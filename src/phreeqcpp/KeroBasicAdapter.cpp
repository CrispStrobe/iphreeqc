#include "KeroBasicAdapter.h"
#include "Phreeqc.h"
#include "NameDouble.h"

#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include <vector>

namespace
{
std::recursive_mutex& my_basic_mutex()
{
	static std::recursive_mutex mutex;
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
			continue;
		}
		if (!quoted)
		{
			bool wb = i == 0 || !std::isalnum(static_cast<unsigned char>(statement[i - 1]));
			struct RuntimeIdentifier { const char* source; const char* target; };
			const RuntimeIdentifier runtime_identifiers[] = {
				{"CELL_NO", "KEROCELLNO"},
				{"SOLN_VOL", "KEROSOLNVOL"},
				{"SIM_TIME", "KEROSIMTIME"},
				{"TOTAL_TIME", "KEROTOTALTIME"}
			};
			bool runtime_replaced = false;
			for (size_t runtime = 0; wb && runtime < sizeof(runtime_identifiers) / sizeof(runtime_identifiers[0]); ++runtime)
			{
				const size_t length = std::char_traits<char>::length(runtime_identifiers[runtime].source);
				bool matches = i + length <= statement.size();
				for (size_t j = 0; matches && j < length; ++j)
				{
					matches = std::toupper(static_cast<unsigned char>(statement[i + j])) == runtime_identifiers[runtime].source[j];
				}
				const bool right = i + length == statement.size() ||
					(!std::isalnum(static_cast<unsigned char>(statement[i + length])) && statement[i + length] != '_');
				if (matches && right)
				{
					result += runtime_identifiers[runtime].target;
					result += "()";
					i += length - 1;
					runtime_replaced = true;
					break;
				}
			}
			if (runtime_replaced) continue;
			struct StringFunction { const char* source; const char* target; };
			const StringFunction string_functions[] = {
				{"STR$", "STR"},
				{"CHR$", "CHR"},
				{"MID$", "KEROMID"},
				{"ARCTAN", "ATAN"},
				{"SQRT", "SQR"}
			};
			bool string_function_replaced = false;
			for (size_t function = 0; wb && function < sizeof(string_functions) / sizeof(string_functions[0]); ++function)
			{
				const size_t length = std::char_traits<char>::length(string_functions[function].source);
				bool matches = i + length <= statement.size();
				for (size_t j = 0; matches && j < length; ++j)
				{
					matches = std::toupper(static_cast<unsigned char>(statement[i + j])) == string_functions[function].source[j];
				}
				if (matches)
				{
					result += string_functions[function].target;
					i += length - 1;
					string_function_replaced = true;
					break;
				}
			}
			if (string_function_replaced) continue;
			const char phase_formula_name[] = "PHASE_FORMULA$";
			bool phase_formula = wb && i + sizeof(phase_formula_name) - 1 <= statement.size();
			for (size_t j = 0; phase_formula && j < sizeof(phase_formula_name) - 1; ++j)
			{
				phase_formula = std::toupper(static_cast<unsigned char>(statement[i + j])) == phase_formula_name[j];
			}
			if (phase_formula)
			{
				result += "KEROPHASEFORMULA";
				i += sizeof(phase_formula_name) - 2;
				continue;
			}
			if (wb && i + 5 <= statement.size() &&
				std::toupper(static_cast<unsigned char>(statement[i])) == 'L' &&
				std::toupper(static_cast<unsigned char>(statement[i + 1])) == 'O' &&
				std::toupper(static_cast<unsigned char>(statement[i + 2])) == 'G' &&
				statement[i + 3] == '1' && statement[i + 4] == '0' &&
				(i + 5 == statement.size() || !std::isalnum(static_cast<unsigned char>(statement[i + 5]))))
			{
				result += "KEROLOG10";
				i += 4;
				continue;
			}
		}
		result += statement[i];
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
	Program() : interpreter(NULL) {}
	struct mb_interpreter_t* interpreter;
};

KeroBasicAdapter::KeroBasicAdapter(Phreeqc* phreeqc)
	: PhreeqcPtr(phreeqc), ActiveProgram(NULL), RecursionDepth(0), OutputBytes(0), Statements(0)
{
	std::lock_guard<std::recursive_mutex> lock(my_basic_mutex());
	if (!my_basic_initialized())
	{
		LastError = "MY-BASIC runtime initialization failed";
	}
}

KeroBasicAdapter::~KeroBasicAdapter()
{
	std::lock_guard<std::recursive_mutex> lock(my_basic_mutex());
	while (!Programs.empty())
	{
		destroy_program(*Programs.begin());
	}
}

int KeroBasicAdapter::basic_compile(const char* commands, void** lnbase, void** vbase, void** lpbase)
{
	std::lock_guard<std::recursive_mutex> lock(my_basic_mutex());
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
		{"DELTAZUNDERSCOREZHZUNDERSCOREZSPECIES", species_delta_h_callback},
		{"KEROLOG10", log10_callback},
		{"LA", log_activity_callback},
		{"KIN", kinetics_moles_callback},
		{"EQUI", equi_phase_callback},
		{"GAS", gas_callback},
		{"SZUNDERSCOREZS", ss_callback},
		{"GFW", gfw_callback},
		{"KEROPHASEFORMULA", phase_formula_callback},
		{"CALCZUNDERSCOREZVALUE", calc_value_callback},
		{"SUMZUNDERSCOREZSPECIES", sum_species_callback},
		{"SUMZUNDERSCOREZGAS", sum_gas_callback},
		{"KEROCELLNO", cell_no_callback},
		{"KEROSOLNVOL", soln_vol_callback},
		{"KEROSIMTIME", sim_time_callback},
		{"KEROTOTALTIME", total_time_callback},
		{"KEROMID", mid_string_callback}
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
	if (!LastError.empty())
	{
		destroy_program(program);
		return report_error("compile");
	}
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
	std::lock_guard<std::recursive_mutex> lock(my_basic_mutex());
	Program* program = static_cast<Program*>(lnbase);
	const bool outermost = RecursionDepth == 0;
	if (outermost) LastError.clear();

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
	if (RecursionDepth >= 50)
	{
		LastError = "recursion budget exceeded";
		return report_error("run");
	}
	if (set_runtime_values(program) != 0) return report_error("runtime variables");

	if (outermost)
	{
		Statements = 0;
		OutputBytes = 0;
		Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	}
	Program* previous = ActiveProgram;
	ActiveProgram = program;
	++RecursionDepth;
	int result = MB_FUNC_ERR;
	try
	{
		result = mb_run(program->interpreter, false);
	}
	catch (...)
	{
		--RecursionDepth;
		ActiveProgram = previous;
		throw;
	}
	--RecursionDepth;
	ActiveProgram = previous;
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
	const bool high_precision = adapter->PhreeqcPtr->current_selected_output != NULL
		? adapter->PhreeqcPtr->current_selected_output->Get_high_precision()
		: adapter->PhreeqcPtr->high_precision;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	while (mb_has_arg(interpreter, local))
	{
		mb_value_t value;
		mb_check(mb_pop_value(interpreter, local, &value));
		if (value.type == MB_DT_INT)
		{
			adapter->PhreeqcPtr->fpunchf_user(
				adapter->PhreeqcPtr->n_user_punch_index++,
				high_precision ? "%20.12e\t" : "%12.4e\t",
				static_cast<double>(value.value.integer));
		}
		else if (value.type == MB_DT_REAL)
		{
			adapter->PhreeqcPtr->fpunchf_user(
				adapter->PhreeqcPtr->n_user_punch_index++,
				high_precision ? "%20.12e\t" : "%12.4e\t",
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

int KeroBasicAdapter::log10_callback(struct mb_interpreter_t* interpreter, void** local)
{
	mb_value_t val;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_pop_value(interpreter, local, &val));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	double x = 0;
	if (val.type == MB_DT_INT)
		x = static_cast<double>(val.value.integer);
	else if (val.type == MB_DT_REAL)
		x = static_cast<double>(val.value.float_point);
	else
		return MB_FUNC_ERR;
	real_t result = static_cast<real_t>(log10(x));
	mb_check(mb_push_real(interpreter, local, result));
	return MB_FUNC_OK;
}


int KeroBasicAdapter::log_activity_callback(struct mb_interpreter_t* interpreter, void** local)
{
	char* name = NULL;
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_pop_string(interpreter, local, &name));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	if (!name) { adapter->LastError = "LA requires a species name"; return MB_FUNC_ERR; }
	LDBLE value = adapter->PhreeqcPtr->log_activity(name);
	mb_check(mb_push_real(interpreter, local, static_cast<real_t>(value)));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::kinetics_moles_callback(struct mb_interpreter_t* interpreter, void** local)
{
	char* name = NULL;
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_pop_string(interpreter, local, &name));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	if (!name) { adapter->LastError = "KIN requires a kinetics name"; return MB_FUNC_ERR; }
	LDBLE value = adapter->PhreeqcPtr->kinetics_moles(name);
	mb_check(mb_push_real(interpreter, local, static_cast<real_t>(value)));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::equi_phase_callback(struct mb_interpreter_t* interpreter, void** local)
{
	char* name = NULL;
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_pop_string(interpreter, local, &name));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	if (!name) { adapter->LastError = "EQUI requires a phase name"; return MB_FUNC_ERR; }
	LDBLE value = adapter->PhreeqcPtr->equi_phase(name);
	mb_check(mb_push_real(interpreter, local, static_cast<real_t>(value)));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::gas_callback(struct mb_interpreter_t* interpreter, void** local)
{
	char* name = NULL;
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_pop_string(interpreter, local, &name));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	if (!name) { adapter->LastError = "GAS requires a component name"; return MB_FUNC_ERR; }
	LDBLE value = adapter->PhreeqcPtr->find_gas_comp(name);
	mb_check(mb_push_real(interpreter, local, static_cast<real_t>(value)));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::ss_callback(struct mb_interpreter_t* interpreter, void** local)
{
	char* name = NULL;
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_pop_string(interpreter, local, &name));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	if (!name) { adapter->LastError = "S_S requires a component name"; return MB_FUNC_ERR; }
	LDBLE value = adapter->PhreeqcPtr->find_ss_comp(name);
	mb_check(mb_push_real(interpreter, local, static_cast<real_t>(value)));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::gfw_callback(struct mb_interpreter_t* interpreter, void** local)
{
	char* name = NULL;
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_pop_string(interpreter, local, &name));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	if (!name) { adapter->LastError = "GFW requires a formula"; return MB_FUNC_ERR; }
	LDBLE gfw = 0;
	if (adapter->PhreeqcPtr->compute_gfw(name, &gfw) == ERROR)
	{
		adapter->LastError = std::string("could not compute GFW for ") + name;
		return MB_FUNC_ERR;
	}
	mb_check(mb_push_real(interpreter, local, static_cast<real_t>(gfw)));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::phase_formula_callback(struct mb_interpreter_t* interpreter, void** local)
{
	char* name = NULL;
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_pop_string(interpreter, local, &name));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	if (!name) { adapter->LastError = "PHASE_FORMULA requires a phase name"; return MB_FUNC_ERR; }
	cxxNameDouble stoichiometry;
	std::string formula = adapter->PhreeqcPtr->phase_formula(name ? name : "", stoichiometry);
	mb_check(mb_push_string(interpreter, local, mb_memdup(formula.c_str(), static_cast<unsigned>(formula.size() + 1))));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::calc_value_callback(struct mb_interpreter_t* interpreter, void** local)
{
	char* name = NULL;
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_pop_string(interpreter, local, &name));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	if (!name) { adapter->LastError = "CALC_VALUE requires a name"; return MB_FUNC_ERR; }
	LDBLE value = adapter->PhreeqcPtr->get_calculate_value(name);
	mb_check(mb_push_real(interpreter, local, static_cast<real_t>(value)));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::sum_species_callback(struct mb_interpreter_t* interpreter, void** local)
{
	char* template_str = NULL;
	char* element_str = NULL;
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_pop_string(interpreter, local, &template_str));
	if (mb_has_arg(interpreter, local))
	{
		mb_check(mb_pop_string(interpreter, local, &element_str));
	}
	mb_check(mb_attempt_close_bracket(interpreter, local));
	if (!template_str) { adapter->LastError = "sum_species requires a template"; return MB_FUNC_ERR; }
	LDBLE value = adapter->PhreeqcPtr->sum_match_species(template_str, element_str);
	mb_check(mb_push_real(interpreter, local, static_cast<real_t>(value)));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::sum_gas_callback(struct mb_interpreter_t* interpreter, void** local)
{
	char* template_str = NULL;
	char* element_str = NULL;
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_pop_string(interpreter, local, &template_str));
	if (mb_has_arg(interpreter, local))
	{
		mb_check(mb_pop_string(interpreter, local, &element_str));
	}
	mb_check(mb_attempt_close_bracket(interpreter, local));
	if (!template_str) { adapter->LastError = "SUM_GAS requires a template"; return MB_FUNC_ERR; }
	LDBLE value = adapter->PhreeqcPtr->sum_match_gases(template_str, element_str);
	mb_check(mb_push_real(interpreter, local, static_cast<real_t>(value)));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::cell_no_callback(struct mb_interpreter_t* interpreter, void** local)
{
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	mb_check(mb_push_real(interpreter, local, static_cast<real_t>(adapter->PhreeqcPtr->solution_number())));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::soln_vol_callback(struct mb_interpreter_t* interpreter, void** local)
{
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	mb_check(mb_push_real(interpreter, local, static_cast<real_t>(adapter->PhreeqcPtr->calc_solution_volume())));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::sim_time_callback(struct mb_interpreter_t* interpreter, void** local)
{
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	mb_check(mb_push_real(interpreter, local, static_cast<real_t>(adapter->sim_time_value())));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::total_time_callback(struct mb_interpreter_t* interpreter, void** local)
{
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	mb_check(mb_push_real(interpreter, local, static_cast<real_t>(adapter->total_time_value())));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::mid_string_callback(struct mb_interpreter_t* interpreter, void** local)
{
	char* text = NULL;
	int_t start = 0;
	int_t count = 0;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_pop_string(interpreter, local, &text));
	mb_check(mb_pop_int(interpreter, local, &start));
	if (!text) return MB_FUNC_ERR;
	count = static_cast<int_t>(strlen(text));
	if (mb_has_arg(interpreter, local))
	{
		mb_check(mb_pop_int(interpreter, local, &count));
	}
	mb_check(mb_attempt_close_bracket(interpreter, local));
	if (start < 1) start = 1;
	const size_t offset = static_cast<size_t>(start - 1);
	if (offset > strlen(text) || count < 0) return MB_FUNC_ERR;
	std::string result(text);
	result = result.substr(offset, static_cast<size_t>(count));
	mb_check(mb_push_string(interpreter, local,
		mb_memdup(result.c_str(), static_cast<unsigned>(result.size() + 1))));
	return MB_FUNC_OK;
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
	const size_t emitted = length <= 0 ? 0 :
		(static_cast<size_t>(length) < sizeof(buffer) ? static_cast<size_t>(length) : sizeof(buffer) - 1);
	adapter->OutputBytes += emitted;
	if (adapter->OutputBytes > 1048576)
	{
		adapter->LastError = "output budget exceeded";
		mb_schedule_suspend(interpreter, MB_EXTENDED_ABORT);
		return 0;
	}
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
	if (adapter->LastError == "output budget exceeded") return MB_FUNC_ERR;
	++adapter->Statements;
	if (adapter->Statements > 100000)
	{
		adapter->LastError = "statement budget exceeded";
		return MB_FUNC_ERR;
	}
	if ((adapter->Statements & 255) == 0 &&
		std::chrono::steady_clock::now() > adapter->Deadline)
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

	// PHREEQC's bundled K-feldspar and albite rates use DATA/RESTORE/READ as a
	// straight-line initialization prefix. Resolve that deliberately narrow
	// pattern here; reject control flow into it instead of emulating different
	// runtime cursor semantics silently.
	std::map<std::string, std::vector<std::string> > data_blocks;
	std::set<std::string> data_prefix_lines;
	bool saw_data_prefix = false;
	bool executable_before_data_prefix = false;
	bool data_prefix_closed = false;
	{
		std::istringstream scan(normalized);
		std::string scan_line;
		while (std::getline(scan, scan_line))
		{
			if (!scan_line.empty() && scan_line[scan_line.size() - 1] == '\r')
				scan_line.erase(scan_line.size() - 1);
			std::string::size_type first = scan_line.find_first_not_of(" \t");
			if (first == std::string::npos) continue;
			std::string::size_type digit = first;
			while (digit < scan_line.size() && std::isdigit(static_cast<unsigned char>(scan_line[digit]))) ++digit;
			if (digit <= first) continue;
			std::string line_num = scan_line.substr(first, digit - first);
			std::string::size_type body_start = scan_line.find_first_not_of(" \t", digit);
			if (body_start == std::string::npos) continue;
			std::string body = scan_line.substr(body_start);
			const bool is_data = starts_with_word(body, "DATA");
			const bool is_restore = starts_with_word(body, "RESTORE");
			const bool is_read = starts_with_word(body, "READ");
			const bool is_data_statement = is_data || is_restore || is_read;
			if (is_data_statement)
			{
				if (executable_before_data_prefix || data_prefix_closed)
				{
					LastError = "DATA/READ/RESTORE is supported only as a straight-line initialization prefix";
					return std::string();
				}
				saw_data_prefix = true;
				data_prefix_lines.insert(line_num);
			}
			else if (!starts_with_word(body, "REM"))
			{
				if (saw_data_prefix)
					data_prefix_closed = true;
				else
					executable_before_data_prefix = true;
			}
			if (!is_data) continue;
			std::string values_str = body.substr(4);
			std::vector<std::string>& values = data_blocks[line_num];
			std::string current;
			bool in_quote = false;
			for (std::string::size_type i = 0; i < values_str.size(); ++i)
			{
				if (values_str[i] == '"') in_quote = !in_quote;
				if (!in_quote && values_str[i] == ',')
				{
					std::string::size_type s = current.find_first_not_of(" \t");
					std::string::size_type e = current.find_last_not_of(" \t");
					values.push_back(s == std::string::npos ? "" : current.substr(s, e - s + 1));
					current.clear();
				}
				else
				{
					current += values_str[i];
				}
			}
			std::string::size_type s = current.find_first_not_of(" \t");
			std::string::size_type e = current.find_last_not_of(" \t");
			if (s != std::string::npos) values.push_back(current.substr(s, e - s + 1));
		}
	}
	if (saw_data_prefix)
	{
		std::istringstream scan(normalized);
		std::string scan_line;
		while (std::getline(scan, scan_line))
		{
			std::string::size_type body_start = scan_line.find_first_not_of(" \t");
			while (body_start != std::string::npos && body_start < scan_line.size() &&
				std::isdigit(static_cast<unsigned char>(scan_line[body_start]))) ++body_start;
			body_start = scan_line.find_first_not_of(" \t", body_start);
			if (body_start == std::string::npos) continue;
			std::string body = scan_line.substr(body_start);
			if (starts_with_word(body, "REM")) continue;
			bool quoted = false;
			for (std::string::size_type pos = 0; pos < body.size(); ++pos)
			{
				if (body[pos] == '"')
				{
					quoted = !quoted;
					continue;
				}
				const bool left = pos == 0 || !std::isalnum(static_cast<unsigned char>(body[pos - 1]));
				if (quoted || !left) continue;
				const bool gosub = starts_with_word(body.substr(pos), "GOSUB");
				const bool go_to = starts_with_word(body.substr(pos), "GOTO");
				if (!gosub && !go_to) continue;
				std::string::size_type target = pos + (gosub ? 5 : 4);
				while (target < body.size() && std::isspace(static_cast<unsigned char>(body[target]))) ++target;
				std::string digits;
				while (target < body.size() && std::isdigit(static_cast<unsigned char>(body[target])))
					digits += body[target++];
				if (!digits.empty() && data_prefix_lines.find(digits) != data_prefix_lines.end())
				{
					LastError = "control flow into a DATA initialization prefix is not supported";
					return std::string();
				}
				pos = target;
			}
		}
	}

	// Second pass: transform lines, resolving RESTORE/READ against collected DATA
	std::string active_data_line;
	size_t active_data_offset = 0;
	std::istringstream input(normalized);
	std::ostringstream output;
	std::string line;
	while (std::getline(input, line))
	{
		if (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);
		std::string::size_type first = line.find_first_not_of(" \t");
		std::string::size_type digit = first;
		while (digit != std::string::npos && digit < line.size() && std::isdigit(static_cast<unsigned char>(line[digit]))) ++digit;
		std::string line_num;
		if (first != std::string::npos && digit > first && (digit == line.size() || std::isspace(static_cast<unsigned char>(line[digit]))))
		{
			line_num = line.substr(first, digit - first);
			output << line_label(line_num) << ":\n";
			line = digit == line.size() ? std::string() : line.substr(digit);
		}

		std::string::size_type body_first = line.find_first_not_of(" \t");
		if (body_first != std::string::npos)
		{
			std::string body = line.substr(body_first);
			if (starts_with_word(body, "DATA"))
			{
				output << "REM DATA\n";
				continue;
			}
			if (starts_with_word(body, "RESTORE"))
			{
				std::string rest = body.substr(7);
				std::string::size_type rs = rest.find_first_not_of(" \t");
				if (rs != std::string::npos)
				{
					std::string target;
					while (rs < rest.size() && std::isdigit(static_cast<unsigned char>(rest[rs])))
						target += rest[rs++];
					if (!target.empty())
					{
						if (data_blocks.find(target) == data_blocks.end())
						{
							LastError = std::string("RESTORE target has no DATA line: ") + target;
							return std::string();
						}
						active_data_line = target;
						active_data_offset = 0;
					}
				}
				output << "REM RESTORE\n";
				continue;
			}
			if (starts_with_word(body, "READ"))
			{
				if (active_data_line.empty())
				{
					LastError = "READ requires a preceding RESTORE in the supported initialization prefix";
					return std::string();
				}
				std::string vars_str = body.substr(4);
				std::vector<std::string> vars;
				std::string current;
				for (std::string::size_type i = 0; i < vars_str.size(); ++i)
				{
					if (vars_str[i] == ',')
					{
						std::string::size_type vs = current.find_first_not_of(" \t");
						std::string::size_type ve = current.find_last_not_of(" \t");
						if (vs != std::string::npos) vars.push_back(current.substr(vs, ve - vs + 1));
						current.clear();
					}
					else
					{
						current += vars_str[i];
					}
				}
				std::string::size_type vs = current.find_first_not_of(" \t");
				std::string::size_type ve = current.find_last_not_of(" \t");
				if (vs != std::string::npos) vars.push_back(current.substr(vs, ve - vs + 1));

				std::map<std::string, std::vector<std::string> >::iterator it = data_blocks.find(active_data_line);
				for (size_t i = 0; i < vars.size(); ++i)
				{
					if (it == data_blocks.end() || active_data_offset >= it->second.size())
					{
						LastError = "READ exhausted the supported DATA initialization prefix";
						return std::string();
					}
					std::string val = it->second[active_data_offset++];
					output << translate_identifiers(vars[i]) << " = " << val << "\n";
				}
				continue;
			}
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
	Value values[] = {
		{"M", PhreeqcPtr->rate_m},
		{"M0", PhreeqcPtr->rate_m0},
		{"TIME", PhreeqcPtr->rate_time},
		{"TC", PhreeqcPtr->tc_x},
		{"TK", PhreeqcPtr->tc_x + 273.15}
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

double KeroBasicAdapter::sim_time_value() const
{
	LDBLE sim_time = 0;
	if (!PhreeqcPtr->use.Get_kinetics_in())
	{
		if (PhreeqcPtr->state == PHAST)
		{
			sim_time = PhreeqcPtr->rate_sim_time;
		}
		else if (PhreeqcPtr->state == TRANSPORT)
		{
			sim_time = PhreeqcPtr->transport_step * PhreeqcPtr->timest;
		}
		else if (PhreeqcPtr->state == ADVECTION)
		{
			sim_time = PhreeqcPtr->advection_kin_time_defined == TRUE
				? PhreeqcPtr->advection_step * PhreeqcPtr->advection_kin_time
				: static_cast<LDBLE>(PhreeqcPtr->advection_step);
		}
	}
	else
	{
		sim_time = PhreeqcPtr->rate_sim_time;
	}
	return sim_time;
}

double KeroBasicAdapter::total_time_value() const
{
	if (!PhreeqcPtr->use.Get_kinetics_in())
	{
		if (PhreeqcPtr->state == PHAST)
		{
			return PhreeqcPtr->rate_sim_time_end;
		}
		if (PhreeqcPtr->state == TRANSPORT)
			return PhreeqcPtr->initial_total_time + PhreeqcPtr->transport_step * PhreeqcPtr->timest;
		if (PhreeqcPtr->state == ADVECTION)
			return PhreeqcPtr->initial_total_time +
				PhreeqcPtr->advection_step * PhreeqcPtr->advection_kin_time;
		return 0;
	}
	return PhreeqcPtr->initial_total_time + PhreeqcPtr->rate_sim_time;
}

void KeroBasicAdapter::destroy_program(Program* program)
{
	if (!program) return;
	Programs.erase(program);
	if (program->interpreter) mb_close(&program->interpreter);
	delete program;
}
