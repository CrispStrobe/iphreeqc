#include "KeroBasicAdapter.h"
#include "Phreeqc.h"

#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <sstream>

namespace
{
std::mutex& my_basic_mutex()
{
	static std::mutex mutex;
	return mutex;
}

bool my_basic_initialized()
{
	struct Runtime
	{
		Runtime() : initialized(mb_init() == MB_FUNC_OK) {}
		~Runtime()
		{
			if (initialized) mb_dispose();
		}
		bool initialized;
	};
	static Runtime runtime;
	return runtime.initialized;
}

std::string trim_copy(const char* text)
{
	std::string value = text ? text : "";
	std::string::size_type first = value.find_first_not_of(" \t\r\n;");
	if (first == std::string::npos) return std::string();
	std::string::size_type last = value.find_last_not_of(" \t\r\n;");
	value = value.substr(first, last - first + 1);
	for (std::string::iterator it = value.begin(); it != value.end(); ++it)
	{
		*it = static_cast<char>(std::tolower(static_cast<unsigned char>(*it)));
	}
	return value;
}

bool starts_with_word(const std::string& text, const char* word)
{
	std::string::size_type length = std::char_traits<char>::length(word);
	if (text.size() < length) return false;
	for (std::string::size_type i = 0; i < length; ++i)
	{
		if (std::toupper(static_cast<unsigned char>(text[i])) != word[i]) return false;
	}
	return text.size() == length || std::isspace(static_cast<unsigned char>(text[length]));
}

std::string line_label(const std::string& digits)
{
	std::string label = "KEROLINE";
	for (std::string::const_iterator it = digits.begin(); it != digits.end(); ++it)
	{
		label += static_cast<char>('A' + (*it - '0'));
	}
	return label;
}

std::string translate_jump_targets(const std::string& statement)
{
	std::string result;
	bool quoted = false;
	for (std::string::size_type i = 0; i < statement.size();)
	{
		if (statement[i] == '"')
		{
			quoted = !quoted;
			result += statement[i++];
			continue;
		}
		bool word_boundary = i == 0 || !std::isalnum(static_cast<unsigned char>(statement[i - 1]));
		if (!quoted && word_boundary &&
			(starts_with_word(statement.substr(i), "GOTO") || starts_with_word(statement.substr(i), "GOSUB")))
		{
			std::string::size_type word_length = starts_with_word(statement.substr(i), "GOSUB") ? 5 : 4;
			result.append(statement, i, word_length);
			i += word_length;
			while (i < statement.size() && std::isspace(static_cast<unsigned char>(statement[i]))) result += statement[i++];
			std::string::size_type target = i;
			while (i < statement.size() && std::isdigit(static_cast<unsigned char>(statement[i]))) ++i;
			if (i > target) result += line_label(statement.substr(target, i - target));
			continue;
		}
		result += statement[i++];
	}
	return result;
}

std::string translate_identifiers(const std::string& statement)
{
	std::string result;
	bool quoted = false;
	for (std::string::size_type i = 0; i < statement.size(); ++i)
	{
		if (statement[i] == '"') quoted = !quoted;
		if (!quoted && statement[i] == '_')
		{
			result += "ZUNDERSCOREZ";
		}
		else
		{
			result += statement[i];
		}
	}
	return result;
}

std::string transform_statement(const std::string& statement)
{
	std::string::size_type first = statement.find_first_not_of(" \t");
	if (first == std::string::npos) return statement;
	std::string prefix = statement.substr(0, first);
	std::string body = statement.substr(first);
	if (starts_with_word(body, "REM")) return statement;
	if (starts_with_word(body, "SAVE"))
	{
		std::string expression = body.substr(4);
		return prefix + "KEROSAVE(" + translate_identifiers(expression) + ")";
	}
	if (starts_with_word(body, "PUNCH"))
	{
		std::string expressions = body.substr(5);
		return prefix + "KEROPUNCH(" + translate_identifiers(expressions) + ")";
	}
	return prefix + translate_identifiers(translate_jump_targets(body));
}
}

struct KeroBasicAdapter::Program
{
	Program() : interpreter(NULL), statements(0) {}
	struct mb_interpreter_t* interpreter;
	size_t statements;
	std::chrono::steady_clock::time_point deadline;
};

KeroBasicAdapter::KeroBasicAdapter(Phreeqc* phreeqc)
	: PhreeqcPtr(phreeqc), ActiveProgram(NULL)
{
	std::lock_guard<std::mutex> lock(my_basic_mutex());
	if (!my_basic_initialized())
	{
		LastError = "MY-BASIC runtime initialization failed";
	}
}

KeroBasicAdapter::~KeroBasicAdapter()
{
	std::lock_guard<std::mutex> lock(my_basic_mutex());
	while (!Programs.empty())
	{
		destroy_program(*Programs.begin());
	}
}

int KeroBasicAdapter::basic_compile(const char* commands, void** lnbase, void** vbase, void** lpbase)
{
	std::lock_guard<std::mutex> lock(my_basic_mutex());
	if (lnbase) *lnbase = NULL;
	if (vbase) *vbase = NULL;
	if (lpbase) *lpbase = NULL;
	LastError.clear();

	if (!commands || !lnbase || !vbase || !lpbase)
	{
		LastError = "invalid compile arguments";
		return report_error("compile");
	}
	if (!my_basic_initialized())
	{
		LastError = "MY-BASIC runtime initialization failed";
		return report_error("compile");
	}

	Program* program = new Program();
	if (mb_open(&program->interpreter) != MB_FUNC_OK || !program->interpreter)
	{
		delete program;
		LastError = "could not create interpreter";
		return report_error("compile");
	}
	mb_set_userdata(program->interpreter, this);
	mb_set_printer(program->interpreter, print_callback);
	mb_set_inputer(program->interpreter, input_callback);
	mb_set_import_handler(program->interpreter, import_callback);
	mb_set_error_handler(program->interpreter, error_callback);
	mb_debug_set_stepped_handler(program->interpreter, step_callback, NULL);
	if (mb_register_func(program->interpreter, "KEROSAVE", save_callback) == 0)
	{
		destroy_program(program);
		LastError = "could not register SAVE callback";
		return report_error("compile");
	}
	if (mb_register_func(program->interpreter, "KEROPUNCH", punch_callback) == 0)
	{
		destroy_program(program);
		LastError = "could not register PUNCH callback";
		return report_error("compile");
	}
	struct Callback
	{
		const char* name;
		mb_func_t callback;
	};
	const Callback callbacks[] = {
		{"PARM", parm_callback},
		{"ACT", activity_callback},
		{"MOL", molality_callback},
		{"TOT", total_callback},
		{"SI", saturation_index_callback},
		{"SR", saturation_ratio_callback},
		{"LM", log_molality_callback},
		{"DELTAZUNDERSCOREZHZUNDERSCOREZSPECIES", species_delta_h_callback}
	};
	for (size_t i = 0; i < sizeof(callbacks) / sizeof(callbacks[0]); ++i)
	{
		if (mb_register_func(program->interpreter, callbacks[i].name, callbacks[i].callback) == 0)
		{
			destroy_program(program);
			LastError = std::string("could not register ") + callbacks[i].name;
			return report_error("compile");
		}
	}

	std::string source = transform_source(commands);
	if (mb_load_string(program->interpreter, source.c_str(), true) != MB_FUNC_OK)
	{
		destroy_program(program);
		return report_error("compile");
	}

	Programs.insert(program);
	*lnbase = program;
	return 0;
}

int KeroBasicAdapter::basic_run(char* commands, void* lnbase, void*, void*)
{
	std::lock_guard<std::mutex> lock(my_basic_mutex());
	Program* program = static_cast<Program*>(lnbase);
	LastError.clear();

	if (is_dispose_command(commands))
	{
		if (program && Programs.find(program) != Programs.end()) destroy_program(program);
		return 0;
	}
	if (!program || Programs.find(program) == Programs.end())
	{
		LastError = "unknown compiled program";
		return report_error("run");
	}
	if (set_runtime_values(program) != 0) return report_error("runtime variables");

	program->statements = 0;
	program->deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	ActiveProgram = program;
	int result = mb_run(program->interpreter, false);
	ActiveProgram = NULL;
	if (result != MB_FUNC_OK)
	{
		return report_error(result == MB_FUNC_SUSPEND ? "execution suspended" : "run");
	}
	return 0;
}

int KeroBasicAdapter::save_callback(struct mb_interpreter_t* interpreter, void** local)
{
	mb_value_t value;
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_pop_value(interpreter, local, &value));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	if (value.type == MB_DT_INT)
	{
		adapter->PhreeqcPtr->rate_moles = static_cast<LDBLE>(value.value.integer);
	}
	else if (value.type == MB_DT_REAL)
	{
		adapter->PhreeqcPtr->rate_moles = static_cast<LDBLE>(value.value.float_point);
	}
	else
	{
		adapter->LastError = "SAVE requires a numeric value";
		return MB_FUNC_ERR;
	}
	mb_check(mb_push_value(interpreter, local, value));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::punch_callback(struct mb_interpreter_t* interpreter, void** local)
{
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	while (mb_has_arg(interpreter, local))
	{
		mb_value_t value;
		mb_check(mb_pop_value(interpreter, local, &value));
		if (value.type == MB_DT_INT)
		{
			adapter->PhreeqcPtr->fpunchf_user(
				adapter->PhreeqcPtr->n_user_punch_index++,
				adapter->PhreeqcPtr->high_precision ? "%20.12e\t" : "%12.4e\t",
				static_cast<double>(value.value.integer));
		}
		else if (value.type == MB_DT_REAL)
		{
			adapter->PhreeqcPtr->fpunchf_user(
				adapter->PhreeqcPtr->n_user_punch_index++,
				adapter->PhreeqcPtr->high_precision ? "%20.12e\t" : "%12.4e\t",
				static_cast<double>(value.value.float_point));
		}
		else if (value.type == MB_DT_STRING)
		{
			adapter->PhreeqcPtr->fpunchf_user(
				adapter->PhreeqcPtr->n_user_punch_index++,
				"%s\t",
				value.value.string);
		}
		else
		{
			adapter->LastError = "PUNCH supports only numeric and string values";
			return MB_FUNC_ERR;
		}
	}
	mb_check(mb_attempt_close_bracket(interpreter, local));
	mb_check(mb_push_int(interpreter, local, 0));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::parm_callback(struct mb_interpreter_t* interpreter, void** local)
{
	int_t index = 0;
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_pop_int(interpreter, local, &index));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	if (index < 1 || index > adapter->PhreeqcPtr->count_rate_p ||
		static_cast<size_t>(index) > adapter->PhreeqcPtr->rate_p.size())
	{
		adapter->LastError = "PARM index is out of range";
		return MB_FUNC_ERR;
	}
	mb_check(mb_push_real(
		interpreter,
		local,
		static_cast<real_t>(adapter->PhreeqcPtr->rate_p[static_cast<size_t>(index - 1)])));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::activity_callback(struct mb_interpreter_t* interpreter, void** local)
{
	return named_chemistry_callback(interpreter, local, VALUE_ACTIVITY);
}

int KeroBasicAdapter::molality_callback(struct mb_interpreter_t* interpreter, void** local)
{
	return named_chemistry_callback(interpreter, local, VALUE_MOLALITY);
}

int KeroBasicAdapter::total_callback(struct mb_interpreter_t* interpreter, void** local)
{
	return named_chemistry_callback(interpreter, local, VALUE_TOTAL);
}

int KeroBasicAdapter::saturation_index_callback(struct mb_interpreter_t* interpreter, void** local)
{
	return named_chemistry_callback(interpreter, local, VALUE_SATURATION_INDEX);
}

int KeroBasicAdapter::saturation_ratio_callback(struct mb_interpreter_t* interpreter, void** local)
{
	return named_chemistry_callback(interpreter, local, VALUE_SATURATION_RATIO);
}

int KeroBasicAdapter::log_molality_callback(struct mb_interpreter_t* interpreter, void** local)
{
	return named_chemistry_callback(interpreter, local, VALUE_LOG_MOLALITY);
}

int KeroBasicAdapter::species_delta_h_callback(struct mb_interpreter_t* interpreter, void** local)
{
	return named_chemistry_callback(interpreter, local, VALUE_SPECIES_DELTA_H);
}

int KeroBasicAdapter::print_callback(struct mb_interpreter_t* interpreter, const char* format, ...)
{
	char buffer[4096];
	va_list args;
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter || !format) return 0;
	va_start(args, format);
	int length = vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	buffer[sizeof(buffer) - 1] = '\0';
	adapter->PhreeqcPtr->output_msg(buffer);
	return length;
}

int KeroBasicAdapter::input_callback(struct mb_interpreter_t*, const char*, char* buffer, int length)
{
	if (buffer && length > 0) buffer[0] = '\0';
	return 0;
}

int KeroBasicAdapter::import_callback(struct mb_interpreter_t*, const char*)
{
	return MB_FUNC_ERR;
}

void KeroBasicAdapter::error_callback(
	struct mb_interpreter_t* interpreter,
	mb_error_e,
	const char* description,
	const char*,
	int,
	unsigned short row,
	unsigned short column,
	int)
{
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return;
	if (!adapter->LastError.empty()) return;
	std::ostringstream message;
	message << (description ? description : "MY-BASIC error") << " at " << row << ':' << column;
	adapter->LastError = message.str();
}

int KeroBasicAdapter::step_callback(
	struct mb_interpreter_t* interpreter,
	void**,
	const char*,
	int,
	unsigned short,
	unsigned short)
{
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter || !adapter->ActiveProgram) return MB_FUNC_ERR;
	Program* program = adapter->ActiveProgram;
	++program->statements;
	if (program->statements > 100000)
	{
		adapter->LastError = "statement budget exceeded";
		return MB_FUNC_ERR;
	}
	if ((program->statements & 255) == 0 &&
		std::chrono::steady_clock::now() > program->deadline)
	{
		adapter->LastError = "wall-clock budget exceeded";
		return MB_FUNC_ERR;
	}
	return MB_FUNC_OK;
}

std::string KeroBasicAdapter::transform_source(const char* commands)
{
	std::string normalized;
	bool quoted = false;
	for (const char* cursor = commands ? commands : ""; *cursor; ++cursor)
	{
		if (*cursor == '"') quoted = !quoted;
		if (!quoted && *cursor == ';')
		{
			normalized += '\n';
		}
		else if (*cursor != '\r')
		{
			normalized += *cursor;
		}
	}
	std::istringstream input(normalized);
	std::ostringstream output;
	std::string line;
	while (std::getline(input, line))
	{
		if (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);
		std::string::size_type first = line.find_first_not_of(" \t");
		std::string::size_type digit = first;
		while (digit != std::string::npos && digit < line.size() && std::isdigit(static_cast<unsigned char>(line[digit]))) ++digit;
		if (first != std::string::npos && digit > first && (digit == line.size() || std::isspace(static_cast<unsigned char>(line[digit]))))
		{
			output << line_label(line.substr(first, digit - first)) << ":\n";
			line = digit == line.size() ? std::string() : line.substr(digit);
		}
		output << transform_statement(line) << '\n';
	}
	return output.str();
}

bool KeroBasicAdapter::is_dispose_command(const char* commands)
{
	return trim_copy(commands) == "new; quit" || trim_copy(commands) == "new";
}

KeroBasicAdapter* KeroBasicAdapter::from_interpreter(struct mb_interpreter_t* interpreter)
{
	void* userdata = NULL;
	if (!interpreter || mb_get_userdata(interpreter, &userdata) != MB_FUNC_OK) return NULL;
	return static_cast<KeroBasicAdapter*>(userdata);
}

int KeroBasicAdapter::named_chemistry_callback(
	struct mb_interpreter_t* interpreter,
	void** local,
	ChemistryValue kind)
{
	char* name = NULL;
	LDBLE value = 0;
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_pop_string(interpreter, local, &name));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	if (!name)
	{
		adapter->LastError = "chemistry function requires a name";
		return MB_FUNC_ERR;
	}
	switch (kind)
	{
	case VALUE_ACTIVITY:
		value = adapter->PhreeqcPtr->activity(name);
		break;
	case VALUE_MOLALITY:
		value = adapter->PhreeqcPtr->molality(name);
		break;
	case VALUE_TOTAL:
		value = adapter->PhreeqcPtr->total(name);
		break;
	case VALUE_SATURATION_INDEX:
		{
			LDBLE iap = 0;
			if (adapter->PhreeqcPtr->saturation_index(name, &iap, &value) != OK)
			{
				adapter->LastError = std::string("could not calculate SI for ") + name;
				return MB_FUNC_ERR;
			}
		}
		break;
	case VALUE_SATURATION_RATIO:
		value = adapter->PhreeqcPtr->saturation_ratio(name);
		break;
	case VALUE_LOG_MOLALITY:
		value = adapter->PhreeqcPtr->log_molality(name);
		break;
	case VALUE_SPECIES_DELTA_H:
		value = adapter->PhreeqcPtr->calc_deltah_s(name);
		break;
	}
	mb_check(mb_push_real(interpreter, local, static_cast<real_t>(value)));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::report_error(const std::string& context)
{
	std::string message = "MY-BASIC compatibility " + context + " failed";
	if (!LastError.empty()) message += ": " + LastError;
	PhreeqcPtr->error_msg(message.c_str(), CONTINUE);
	return 1;
}

int KeroBasicAdapter::set_runtime_values(Program* program)
{
	struct Value
	{
		const char* name;
		LDBLE value;
	};
	const Value values[] = {
		{"M", PhreeqcPtr->rate_m},
		{"M0", PhreeqcPtr->rate_m0},
		{"TIME", PhreeqcPtr->rate_time},
		{"TC", PhreeqcPtr->tc_x},
		{"TK", PhreeqcPtr->tk_x}
	};
	for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
	{
		mb_value_t value;
		mb_make_real(value, static_cast<real_t>(values[i].value));
		if (mb_add_var(program->interpreter, NULL, values[i].name, value, true) != MB_FUNC_OK)
		{
			LastError = std::string("could not set ") + values[i].name;
			return 1;
		}
	}
	return 0;
}

void KeroBasicAdapter::destroy_program(Program* program)
{
	if (!program) return;
	Programs.erase(program);
	if (program->interpreter) mb_close(&program->interpreter);
	delete program;
}
