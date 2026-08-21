// ChartHandler.cpp: implementation of the ChartHandler class.
//
//////////////////////////////////////////////////////////////////////
#if defined MULTICHART
#include "Phreeqc.h"
#ifdef _DEBUG
#pragma warning(disable : 4786)	// disable truncation warning (Only used by debugger)
#endif
#include "ChartHandler.h"
#include <algorithm>
#include <iostream>
#include <iomanip>

namespace
{
void json_string(std::ostream& out, const std::string& value)
{
	out << '"';
	for (std::string::const_iterator it = value.begin(); it != value.end(); ++it)
	{
		switch (*it)
		{
		case '"': out << "\\\""; break;
		case '\\': out << "\\\\"; break;
		case '\b': out << "\\b"; break;
		case '\f': out << "\\f"; break;
		case '\n': out << "\\n"; break;
		case '\r': out << "\\r"; break;
		case '\t': out << "\\t"; break;
		default:
			if (static_cast<unsigned char>(*it) < 0x20)
				out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
					<< static_cast<unsigned>(static_cast<unsigned char>(*it))
					<< std::dec << std::setfill(' ');
			else out << *it;
		}
	}
	out << '"';
}
}

#if defined(PHREEQCI_GUI)
#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif
#endif

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

ChartHandler::ChartHandler(PHRQ_io *io)
:
PHRQ_base(io)
	//
	// default constructor for ChartHandler
	//
{
	current_chart = NULL;
	current_chart_n_user = -1000;
	u_g_defined = false;
	timer = true;
	active_charts = 0;
}

ChartHandler::~ChartHandler()
{
	std::map<int, ChartObject *>::iterator it;
	for (it = this->chart_map.begin(); it != chart_map.end(); it++)
	{
		delete it->second;
	}
}
void
ChartHandler::Punch_user_graph(Phreeqc * phreeqc_ptr)
{
	std::map<int, ChartObject *>::iterator it = this->chart_map.begin();
	for ( ; it != chart_map.end(); it++)
	{
		if (it->second->Get_active())
		{
#if defined(__cplusplus_cli)
			while (0 != System::Threading::Interlocked::CompareExchange(it->second->usingResource, 4, 0))
			{
				System::Threading::Thread::Sleep(5);
			}
#endif
			try
			{
				this->current_chart = it->second;
				phreeqc_ptr-> punch_user_graph();
			}
			catch (...)
			{
#if defined(__cplusplus_cli)
				int n = System::Threading::Interlocked::Exchange(it->second->usingResource, 0);
				assert(n == 4);
#endif
				throw;
			}
#if defined(__cplusplus_cli)
			System::Threading::Interlocked::Exchange(it->second->usingResource, 0);
#endif
		}
	}
}

bool
ChartHandler::Read(Phreeqc * phreeqc_ptr, CParser &parser)
{
	int n_user;
	std::string token;

	// reads line, next character is after keyword
	parser.check_line("ChartHandler", true, false, true, false);

	std::istringstream iss(parser.line());
	// keyword
	iss >> token;
	// number
	if (!(iss >> n_user))
	{
		n_user = 1;
	}

	// makes new ChartObject if necessary
	std::map<int, ChartObject *>::iterator it = this->chart_map.find(n_user);
	if (it == this->chart_map.end())
	{
		chart_map[n_user] = new ChartObject(this->Get_io());
		it = this->chart_map.find(n_user);
		it->second->Set_phreeqc(phreeqc_ptr);
	}

	// Read/update ChartObject
#if defined(__cplusplus_cli)
	while (0 != System::Threading::Interlocked::CompareExchange(it->second->usingResource, 5, 0))
	{
		System::Threading::Thread::Sleep(5);
	}
#endif
	try
	{
		{
			it->second->Read(parser);
			current_chart_n_user = n_user;
			current_chart = it->second;
			u_g_defined = true;
		}

		// if detached, set timer_end and free
		if (it->second->Get_detach() && it->second->Get_form_started())
		{
			it->second->Set_end_timer(true);
			it->second->Rate_free();
		}
	}
	catch(...)
	{
#if defined(__cplusplus_cli)
		// Release lock
		int n = System::Threading::Interlocked::Exchange(it->second->usingResource, 0);
		assert(n == 5);
		throw;
#endif
	}
#if defined(__cplusplus_cli)
	// Release lock
	int n = System::Threading::Interlocked::Exchange(it->second->usingResource, 0);
	assert(n == 5);
#endif

	// if detached, wait for thread to acknowledge and then erase chart
	if (it->second->Get_detach())
	{
		while (it->second->Get_form_started() && it->second->Get_done() != true) 
		{
#if defined(__cplusplus_cli)
			System::Threading::Thread::Sleep(5);
#endif
		}
		delete it->second;
		this->chart_map.erase(it);
	}	
	return true;
}
bool
ChartHandler::End_timer()
{
	
	size_t max_tries = 6000; // 1 h, but not used
	std::map<int, ChartObject *>::iterator it = this->chart_map.begin();
	if (chart_map.size() > 0) 
	{
		screen_msg("Detaching charts...");
		if (io != NULL)
		{
			io->error_flush();
		}
	}
	for  ( ; it != chart_map.end(); it++)
	{
		it->second->Rate_free();
		if (it->second->Get_form_started())
		{
#if defined(__cplusplus_cli)
			while (0 != System::Threading::Interlocked::CompareExchange(it->second->usingResource, 6, 0))
			{
				System::Threading::Thread::Sleep(60);
			}
#endif
			it->second->Set_end_timer(true);
			//it->second->Set_phreeqc(NULL);
#if defined(__cplusplus_cli)
			int n = System::Threading::Interlocked::Exchange(it->second->usingResource, 0);
			assert(n == 6);
#endif
			while (it->second->Get_done() != true) 
			{
#if defined(__cplusplus_cli)
				System::Threading::Thread::Sleep(60);
#endif
			}
			//if (i >= max_tries || i2 >= max_tries)
			//{
			//	error_msg("\nChart did not respond.", CONTINUE);
			//}
		}
	}
	if (chart_map.size() > 0)
	{
		screen_msg("\rCharts detached.         \n");
		if (io != NULL)
		{
			io->error_flush();
		}
	}
	this->timer = false;

	return true;
}
bool
ChartHandler::dump(std::ostream & oss, unsigned int indent)
{
	std::map<int, ChartObject *>::iterator it = this->chart_map.begin();
	for  ( ; it != chart_map.end(); it++)
	{
		it->second->dump(oss, indent);
	}
	return true;
}

std::string
ChartHandler::ToJson() const
{
	std::ostringstream out;
	out << std::setprecision(17) << "{\"charts\":[";
	bool first_chart = true;
	for (std::map<int, ChartObject *>::const_iterator chart_it = chart_map.begin(); chart_it != chart_map.end(); ++chart_it)
	{
		const ChartObject* chart = chart_it->second;
		if (!chart) continue;
		if (!first_chart) out << ',';
		first_chart = false;
		out << "{\"user_number\":" << chart_it->first << ",\"title\":";
		json_string(out, chart->Get_chart_title());
		out << ",\"axis_titles\":[";
		for (size_t i = 0; i < chart->Get_axis_titles().size(); ++i)
		{
			if (i) out << ',';
			json_string(out, chart->Get_axis_titles()[i]);
		}
		out << "],\"series\":[";
		const std::vector<CurveObject *>& curves = chart->Get_Curves();
		for (size_t i = 0; i < curves.size(); ++i)
		{
			if (i) out << ',';
			const CurveObject* curve = curves[i];
			out << "{\"id\":";
			json_string(out, curve->Get_id());
			out << ",\"color\":";
			json_string(out, curve->Get_color());
			out << ",\"symbol\":";
			json_string(out, curve->Get_symbol());
			out << ",\"line_width\":" << curve->Get_line_w()
				<< ",\"symbol_size\":" << curve->Get_symbol_size()
				<< ",\"y_axis\":" << curve->Get_y_axis() << ",\"points\":[";
			const size_t count = std::min(curve->Get_x().size(), curve->Get_y().size());
			for (size_t point = 0; point < count; ++point)
			{
				if (point) out << ',';
				out << '[' << curve->Get_x()[point] << ',' << curve->Get_y()[point] << ']';
			}
			out << "]}";
		}
		out << "]}";
	}
	out << "]}";
	return out.str();
}
#endif
