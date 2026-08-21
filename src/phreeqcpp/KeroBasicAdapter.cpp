#include "KeroBasicAdapter.h"
#include "Phreeqc.h"
#include "NameDouble.h"
#include "Solution.h"
#include "UserPunch.h"
#if defined(MULTICHART)
#include "ChartObject.h"
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <mutex>
#include <sstream>
#include <vector>

namespace
{
const char KERO_EOL_NOTAB_MARKER[] = "\x1eKEROTAKIS_EOL_NOTAB\x1e";
const char KERO_NO_NEWLINE_MARKER[] = "\x1eKEROTAKIS_NO_NEWLINE\x1e";
const size_t KERO_ARRAY_ELEMENT_BUDGET = 1000000;

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

std::string::size_type find_word_ci(
	const std::string& text,
	const char* word,
	std::string::size_type start = 0)
{
	const std::string::size_type length = std::char_traits<char>::length(word);
	for (std::string::size_type pos = start; pos + length <= text.size(); ++pos)
	{
		const bool left = pos == 0 ||
			(!std::isalnum(static_cast<unsigned char>(text[pos - 1])) && text[pos - 1] != '_' && text[pos - 1] != '$');
		const bool right = pos + length == text.size() ||
			(!std::isalnum(static_cast<unsigned char>(text[pos + length])) && text[pos + length] != '_' && text[pos + length] != '$');
		if (!left || !right) continue;
		bool matches = true;
		for (std::string::size_type i = 0; i < length; ++i)
		{
			if (std::toupper(static_cast<unsigned char>(text[pos + i])) != word[i])
			{
				matches = false;
				break;
			}
		}
		if (matches) return pos;
	}
	return std::string::npos;
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
		if (statement[i] == '"' && (i == 0 || statement[i - 1] != '\\'))
		{
			quoted = !quoted;
			result += statement[i++];
			continue;
		}
		bool word_boundary = i == 0 ||
			(!std::isalnum(static_cast<unsigned char>(statement[i - 1])) && statement[i - 1] != '_');
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
		if (statement[i] == '"' && (i == 0 || statement[i - 1] != '\\')) quoted = !quoted;
		if (!quoted && statement[i] == '_')
		{
			result += "ZUNDERSCOREZ";
			continue;
		}
		if (!quoted)
		{
			bool wb = i == 0 ||
				(!std::isalnum(static_cast<unsigned char>(statement[i - 1])) && statement[i - 1] != '_');
			struct RuntimeIdentifier { const char* source; const char* target; };
			const RuntimeIdentifier runtime_identifiers[] = {
				{"CELL_NO", "KEROCELLNO"},
				{"SOLN_VOL", "KEROSOLNVOL"},
				{"SIM_TIME", "KEROSIMTIME"},
				{"TOTAL_TIME", "KEROTOTALTIME"},
				{"STEP_NO", "KEROSTEPNO"},
				{"SIM_NO", "KEROSIMNO"},
				{"GAS_VM", "KEROGASVM"},
				{"GAS_P", "KEROGASP"},
				{"DIST", "KERODIST"},
				{"RXN", "KERORXN"},
				{"ALK", "KEROALK"},
				{"APHI", "KEROAPHI"},
				{"CHARGE_BALANCE", "KEROCHARGEBALANCE"},
				{"CURRENT_A", "KEROCURRENTA"},
				{"DH_AV", "KERODHAV"},
				{"DH_A", "KERODHA"},
				{"DH_B", "KERODHB"},
				{"EPS_R", "KEROEPSR"},
				{"ITERATIONS", "KEROITERATIONS"},
				{"KAPPA", "KEROKAPPA"},
				{"KIN_TIME", "KEROKINTIME"},
				{"MU", "KEROMU"},
				{"OSMOTIC", "KEROOSMOTIC"},
				{"PERCENT_ERROR", "KEROPERCENTERROR"},
				{"POT_V", "KEROPOTV"},
				{"PRESSURE", "KEROPRESSURE"},
				{"QBRN", "KEROQBRN"},
				{"RHO_0", "KERORHOZERO"},
				{"RHO", "KERORHO"},
				{"SC", "KEROSC"},
				{"TITLE", "KEROTITLE"},
				{"DESCRIPTION", "KERODESCRIPTION"},
				{"DEBYE_LENGTH", "KERODEBYELENGTH"}
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
					(!std::isalnum(static_cast<unsigned char>(statement[i + length])) &&
					 statement[i + length] != '_' && statement[i + length] != '$');
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
				{"KINETICS_FORMULA$", "KEROKINETICSFORMULA"},
				{"KINETICS_FORMULA", "KEROKINETICSFORMULA"},
				{"PHASE_FORMULA$", "KEROPHASEFORMULA"},
				{"PHASE_FORMULA", "KEROPHASEFORMULA"},
				{"SPECIES_FORMULA$", "KEROSPECIESFORMULA"},
				{"SPECIES_FORMULA", "KEROSPECIESFORMULA"},
				{"PHASE_EQUATION$", "KEROPHASEEQUATION"},
				{"PHASE_EQUATION", "KEROPHASEEQUATION"},
				{"SPECIES_EQUATION$", "KEROSPECIESEQUATION"},
				{"SPECIES_EQUATION", "KEROSPECIESEQUATION"},
				{"EOL_NOTAB$", "KEROEOLNOTAB()"},
				{"NO_NEWLINE$", "KERONONEWLINE()"},
				{"STR_E$", "KEROSTRE"},
				{"STR_F$", "KEROSTRF"},
				{"GET$", "KEROGETSTRING"},
				{"GET", "KEROGET"},
				{"EXISTS", "KEROEXISTS"},
				{"STR$", "KEROSTR"},
				{"CHR$", "CHR"},
				{"EOL$", "KEROEOL()"},
				{"TRIM", "KEROTRIM"},
				{"LTRIM", "KEROLTRIM"},
				{"RTRIM", "KERORTRIM"},
				{"ISO_UNIT", "KEROISOUNIT"},
				{"MID$", "KEROMID"},
				{"INSTR", "KEROINSTR"},
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
				const bool right = i + length == statement.size() ||
					(!std::isalnum(static_cast<unsigned char>(statement[i + length])) && statement[i + length] != '_');
				if (matches && right)
				{
					result += string_functions[function].target;
					i += length - 1;
					string_function_replaced = true;
					break;
				}
			}
			if (string_function_replaced) continue;
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

std::string normalize_graph_arguments(const std::string& arguments)
{
	std::string result;
	bool quoted = false;
	int depth = 0;
	for (std::string::size_type i = 0; i < arguments.size();)
	{
		if (arguments[i] == '"' && (i == 0 || arguments[i - 1] != '\\'))
		{
			quoted = !quoted;
			result += arguments[i++];
			continue;
		}
		if (!quoted && arguments[i] == '(') ++depth;
		if (!quoted && arguments[i] == ')') --depth;
		if (!quoted && depth == 0 && std::isspace(static_cast<unsigned char>(arguments[i])))
		{
			std::string::size_type next = i;
			while (next < arguments.size() && std::isspace(static_cast<unsigned char>(arguments[next]))) ++next;
			std::string::size_type name_end = next;
			while (name_end < arguments.size() &&
				(std::isalnum(static_cast<unsigned char>(arguments[name_end])) || arguments[name_end] == '_' || arguments[name_end] == '$'))
				++name_end;
			std::string::size_type after_name = name_end;
			while (after_name < arguments.size() && std::isspace(static_cast<unsigned char>(arguments[after_name]))) ++after_name;
			std::string::size_type previous = result.find_last_not_of(" \t");
			const bool previous_ends_expression = previous != std::string::npos &&
				result[previous] != '+' && result[previous] != '-' && result[previous] != '*' &&
				result[previous] != '/' && result[previous] != '^' && result[previous] != '=' &&
				result[previous] != '<' && result[previous] != '>' && result[previous] != ',' &&
				result[previous] != '(';
			std::string next_word = arguments.substr(next, name_end - next);
			for (std::string::iterator it = next_word.begin(); it != next_word.end(); ++it)
				*it = static_cast<char>(std::toupper(static_cast<unsigned char>(*it)));
			const bool next_is_operator = next_word == "AND" || next_word == "OR" || next_word == "MOD" ||
				next_word == "THEN" || next_word == "ELSE" || next_word == "TO" || next_word == "STEP";
			const bool next_starts_expression = (name_end > next && !next_is_operator) ||
				(next < arguments.size() && (arguments[next] == '"' || arguments[next] == '\''));
			if (previous_ends_expression && next_starts_expression)
			{
				result += ',';
				i = next;
				continue;
			}
		}
		result += arguments[i++];
	}
	return result;
}

std::string rewrite_graph_commands(const std::string& statement, size_t* plot_xy_index)
{
	struct GraphCommand { const char* source; const char* target; bool many; };
	const GraphCommand commands[] = {
		{"GRAPH_X", "KEROGRAPHX", false},
		{"GRAPH_Y", "KEROGRAPHY", true},
		{"GRAPH_SY", "KEROGRAPHSY", true},
		{"PLOT_XY", "KEROPLOTXY", false},
		{"PUT", "KEROPUT", true},
		{"PUT$", "KEROPUTSTRING", true},
		{"CHANGE_POR", "KEROCHANGEPOR", true},
		{"PUNCH", "KEROPUNCH", true}
	};
	std::string result = statement;
	std::string::size_type search = 0;
	while (search < result.size())
	{
		std::string::size_type found = std::string::npos;
		const GraphCommand* command = NULL;
		for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i)
		{
			const std::string::size_type candidate = find_word_ci(result, commands[i].source, search);
			if (candidate != std::string::npos && (found == std::string::npos || candidate < found))
			{
				found = candidate;
				command = &commands[i];
			}
		}
		if (!command) break;
		const std::string::size_type name_length = std::char_traits<char>::length(command->source);
		std::string::size_type expression_start = found + name_length;
		while (expression_start < result.size() && std::isspace(static_cast<unsigned char>(result[expression_start]))) ++expression_start;
		std::string::size_type expression_end = result.size();
		const std::string::size_type else_pos = find_word_ci(result, "ELSE", expression_start);
		const std::string::size_type colon_pos = result.find(':', expression_start);
		if (else_pos != std::string::npos) expression_end = else_pos;
		if (colon_pos != std::string::npos && colon_pos < expression_end) expression_end = colon_pos;
		std::string expression = result.substr(expression_start, expression_end - expression_start);
		std::string::size_type last = expression.find_last_not_of(" \t");
		if (last != std::string::npos) expression.erase(last + 1);
		if ((std::strcmp(command->source, "PUT") == 0 || std::strcmp(command->source, "PUT$") == 0 ||
			std::strcmp(command->source, "CHANGE_POR") == 0) && expression.size() >= 2 &&
			expression[0] == '(' && expression[expression.size() - 1] == ')')
		{
			expression = expression.substr(1, expression.size() - 2);
		}
		if (command->many) expression = normalize_graph_arguments(expression);
		std::string arguments = expression;
		if (std::strcmp(command->source, "PLOT_XY") == 0 && plot_xy_index)
		{
			std::ostringstream indexed;
			indexed << (*plot_xy_index)++ << ',' << expression;
			arguments = indexed.str();
		}
		const std::string replacement = std::string(command->target) + "(" + arguments + ")";
		result.replace(found, expression_end - found, replacement);
		search = found + replacement.size();
	}
	return result;
}

bool numeric_value(const mb_value_t& value, LDBLE& number)
{
	if (value.type == MB_DT_INT)
	{
		number = static_cast<LDBLE>(value.value.integer);
		return true;
	}
	if (value.type == MB_DT_REAL)
	{
		number = static_cast<LDBLE>(value.value.float_point);
		return true;
	}
	return false;
}

class ConstantExpression
{
public:
	explicit ConstantExpression(const std::string& text) : Text(text), Position(0), Valid(true) {}
	bool evaluate(double& value)
	{
		value = expression();
		skip_space();
		return Valid && Position == Text.size() && std::isfinite(value);
	}
private:
	void skip_space()
	{
		while (Position < Text.size() && std::isspace(static_cast<unsigned char>(Text[Position]))) ++Position;
	}
	double expression()
	{
		double value = term();
		for (;;)
		{
			skip_space();
			if (Position >= Text.size() || (Text[Position] != '+' && Text[Position] != '-')) return value;
			const char op = Text[Position++];
			const double rhs = term();
			value = op == '+' ? value + rhs : value - rhs;
		}
	}
	double term()
	{
		double value = factor();
		for (;;)
		{
			skip_space();
			if (Position >= Text.size() || (Text[Position] != '*' && Text[Position] != '/')) return value;
			const char op = Text[Position++];
			const double rhs = factor();
			if (op == '/' && rhs == 0) Valid = false;
			value = op == '*' ? value * rhs : value / rhs;
		}
	}
	double factor()
	{
		skip_space();
		if (Position < Text.size() && (Text[Position] == '+' || Text[Position] == '-'))
		{
			const char sign = Text[Position++];
			const double value = factor();
			return sign == '-' ? -value : value;
		}
		if (Position < Text.size() && Text[Position] == '(')
		{
			++Position;
			const double value = expression();
			skip_space();
			if (Position >= Text.size() || Text[Position] != ')') Valid = false;
			else ++Position;
			return value;
		}
		const char* begin = Text.c_str() + Position;
		char* end = NULL;
		const double value = std::strtod(begin, &end);
		if (!end || end == begin)
		{
			Valid = false;
			return 0;
		}
		Position = static_cast<size_t>(end - Text.c_str());
		return value;
	}
	const std::string& Text;
	size_t Position;
	bool Valid;
};

bool evaluate_constant_expression(const std::string& text, double& value)
{
	ConstantExpression parser(text);
	return parser.evaluate(value);
}

std::string transform_statement(const std::string& statement, size_t* plot_xy_index)
{
	std::string::size_type first = statement.find_first_not_of(" \t");
	if (first == std::string::npos) return statement;
	std::string prefix = statement.substr(0, first);
	std::string body = statement.substr(first);
	if (starts_with_word(body, "REM")) return statement;
	if (starts_with_word(body, "END")) return prefix + "GOTO KEROPROGRAMEND";
	if (starts_with_word(body, "PRINT"))
	{
		std::string expressions = body.substr(5);
		return prefix + "KEROPRINT(" + translate_identifiers(normalize_graph_arguments(expressions)) + ")";
	}
	if (starts_with_word(body, "DIM"))
	{
		std::vector<std::string> declarations;
		std::string current;
		int depth = 0;
		const std::string rest = body.substr(3);
		for (std::string::size_type i = 0; i < rest.size(); ++i)
		{
			if (rest[i] == '(') ++depth;
			if (rest[i] == ')') --depth;
			if (rest[i] == ',' && depth == 0)
			{
				declarations.push_back(current);
				current.clear();
			}
			else
			{
				current += rest[i];
			}
		}
		declarations.push_back(current);

		std::ostringstream rewritten;
		size_t bound_number = 0;
		bool first_output = true;
		for (size_t declaration = 0; declaration < declarations.size(); ++declaration)
		{
			std::string item = declarations[declaration];
			std::string::size_type begin = item.find_first_not_of(" \t");
			std::string::size_type open = item.find('(', begin);
			std::string::size_type close = item.find_last_of(')');
			if (begin == std::string::npos || open == std::string::npos || close == std::string::npos || close <= open)
				return statement;
			std::string bounds = item.substr(open + 1, close - open - 1);
			std::string bound;
			int bound_depth = 0;
			std::vector<std::string> bound_names;
			for (std::string::size_type i = 0; i <= bounds.size(); ++i)
			{
				const bool separator = i == bounds.size() || (bounds[i] == ',' && bound_depth == 0);
				if (separator)
				{
					std::string::size_type bs = bound.find_first_not_of(" \t");
					std::string::size_type be = bound.find_last_not_of(" \t");
					if (bs == std::string::npos) return statement;
					std::ostringstream name;
					name << "KERODIMBOUND" << bound_number++;
					bound_names.push_back(name.str());
					if (!first_output) rewritten << '\n';
					rewritten << prefix << name.str() << " = "
						<< translate_identifiers(bound.substr(bs, be - bs + 1)) << " + 1";
					first_output = false;
					bound.clear();
					continue;
				}
				if (bounds[i] == '(') ++bound_depth;
				if (bounds[i] == ')') --bound_depth;
				bound += bounds[i];
			}
			if (!first_output) rewritten << '\n';
			rewritten << prefix << "KEROCHECKARRAY(";
			for (size_t i = 0; i < bound_names.size(); ++i)
			{
				if (i != 0) rewritten << ',';
				rewritten << bound_names[i];
			}
			rewritten << ')';
			if (!bound_names.empty()) rewritten << '\n';
			rewritten << prefix << "DIM " << translate_identifiers(item.substr(begin, open - begin + 1));
			for (size_t i = 0; i < bound_names.size(); ++i)
			{
				if (i != 0) rewritten << ',';
				rewritten << bound_names[i];
			}
			rewritten << ')';
			first_output = false;
		}
		return rewritten.str();
	}
	if (starts_with_word(body, "SAVE"))
	{
		std::string expression = body.substr(4);
		return prefix + "KEROSAVE(" + translate_identifiers(expression) + ")";
	}
	if (starts_with_word(body, "PUT") ||
		(body.size() > 3 && find_word_ci(body, "PUT") == 0 && body[3] == '('))
	{
		std::string expressions = body.substr(3);
		std::string::size_type first_expression = expressions.find_first_not_of(" \t");
		std::string::size_type last_expression = expressions.find_last_not_of(" \t");
		if (first_expression != std::string::npos && last_expression > first_expression &&
			expressions[first_expression] == '(' && expressions[last_expression] == ')')
			expressions = expressions.substr(first_expression + 1, last_expression - first_expression - 1);
		return prefix + "KEROPUT(" + translate_identifiers(normalize_graph_arguments(expressions)) + ")";
	}
	if (starts_with_word(body, "PUNCH"))
	{
		std::string expressions = body.substr(5);
		return prefix + "KEROPUNCH(" + translate_identifiers(normalize_graph_arguments(expressions)) + ")";
	}
	const std::string::size_type inline_print = find_word_ci(body, "PRINT");
	if (inline_print != std::string::npos)
	{
		return prefix + translate_identifiers(translate_jump_targets(body.substr(0, inline_print))) +
			"KEROPRINT(" + translate_identifiers(normalize_graph_arguments(body.substr(inline_print + 5))) + ")";
	}
	return translate_identifiers(rewrite_graph_commands(prefix + translate_jump_targets(body), plot_xy_index));
}
}

struct KeroBasicAdapter::Program
{
	struct DataValue
	{
		DataValue() : is_string(false), number(0) {}
		bool is_string;
		double number;
		std::string string;
	};
	Program() : interpreter(NULL), data_cursor(0) {}
	struct mb_interpreter_t* interpreter;
	std::vector<DataValue> data;
	size_t data_cursor;
};

KeroBasicAdapter::KeroBasicAdapter(Phreeqc* phreeqc)
	: PhreeqcPtr(phreeqc), ActiveProgram(NULL), RecursionDepth(0), OutputBytes(0), Statements(0), ArrayElements(0)
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
	if (mb_register_func(program->interpreter, "KEROPRINT", line_print_callback) == 0)
	{
		destroy_program(program);
		LastError = "could not register PRINT callback";
		return report_error("compile");
	}
	struct Callback
	{
		const char* name;
		mb_func_t callback;
	};
	const Callback callbacks[] = {
		{"PARM", parm_callback},
		{"KEROGET", get_callback},
		{"KEROPUT", put_callback},
		{"KEROGETSTRING", get_string_callback},
		{"KEROPUTSTRING", put_string_callback},
		{"KEROEXISTS", exists_callback},
		{"KEROEOL", eol_callback},
		{"KEROEOLNOTAB", eol_notab_callback},
		{"KERONONEWLINE", no_newline_callback},
		{"KEROSTR", str_callback},
		{"KEROSTRE", str_e_callback},
		{"KEROSTRF", str_f_callback},
		{"PAD", pad_callback},
		{"KEROTRIM", trim_callback},
		{"KEROLTRIM", ltrim_callback},
		{"KERORTRIM", rtrim_callback},
		{"KEROISOUNIT", iso_unit_callback},
		{"KEROTITLE", title_callback},
		{"KERODESCRIPTION", description_callback},
		{"LISTZUNDERSCOREZSZUNDERSCOREZS", list_ss_callback},
		{"EDLZUNDERSCOREZSPECIES", edl_species_callback},
		{"SYS", system_total_callback},
		{"EQZUNDERSCOREZFRAC", equivalent_fraction_callback},
		{"MEANG", mean_gamma_callback},
		{"KERODEBYELENGTH", debye_length_callback},
		{"ACT", activity_callback},
		{"MOL", molality_callback},
		{"TOT", total_callback},
		{"SI", saturation_index_callback},
		{"SR", saturation_ratio_callback},
		{"LM", log_molality_callback},
		{"DELTAZUNDERSCOREZHZUNDERSCOREZSPECIES", species_delta_h_callback},
		{"DELTAZUNDERSCOREZHZUNDERSCOREZPHASE", phase_delta_h_callback},
		{"DHZUNDERSCOREZA0", dh_a0_callback},
		{"DHZUNDERSCOREZBDOT", dh_bdot_callback},
		{"DIFFZUNDERSCOREZC", diff_c_callback},
		{"EQUIZUNDERSCOREZDELTA", equi_delta_callback},
		{"GAMMA", gamma_callback},
		{"KINZUNDERSCOREZDELTA", kin_delta_callback},
		{"LG", log_gamma_callback},
		{"LKZUNDERSCOREZNAMED", lk_named_callback},
		{"LKZUNDERSCOREZPHASE", lk_phase_callback},
		{"LKZUNDERSCOREZSPECIES", lk_species_callback},
		{"PHASEZUNDERSCOREZVM", phase_vm_callback},
		{"PRZUNDERSCOREZPHI", pr_phi_callback},
		{"TZUNDERSCOREZSC", t_sc_callback},
		{"KEROLOG10", log10_callback},
		{"LA", log_activity_callback},
		{"KIN", kinetics_moles_callback},
		{"EQUI", equi_phase_callback},
		{"GAS", gas_callback},
		{"SZUNDERSCOREZS", ss_callback},
		{"GFW", gfw_callback},
		{"KEROPHASEFORMULA", phase_formula_callback},
		{"KEROKINETICSFORMULA", kinetics_formula_callback},
		{"KEROSPECIESFORMULA", species_formula_callback},
		{"KEROPHASEEQUATION", phase_equation_callback},
		{"KEROSPECIESEQUATION", species_equation_callback},
		{"CALCZUNDERSCOREZVALUE", calc_value_callback},
		{"SUMZUNDERSCOREZSPECIES", sum_species_callback},
		{"SUMZUNDERSCOREZGAS", sum_gas_callback},
		{"SUMZUNDERSCOREZSZUNDERSCOREZS", sum_ss_callback},
		{"SURF", surface_callback},
		{"EDL", edl_callback},
		{"MISC1", misc1_callback},
		{"MISC2", misc2_callback},
		{"ISO", isotope_callback},
		{"PRZUNDERSCOREZP", partial_pressure_callback},
		{"KEROGASP", gas_pressure_callback},
		{"KEROGASVM", gas_molar_volume_callback},
		{"KEROCELLNO", cell_no_callback},
		{"KEROSTEPNO", step_no_callback},
		{"KEROSIMNO", sim_no_callback},
		{"KERODIST", distance_callback},
		{"KERORXN", reaction_increment_callback},
		{"KEROALK", alk_callback},
		{"KEROAPHI", aphi_callback},
		{"KEROCHARGEBALANCE", charge_balance_callback},
		{"KEROCURRENTA", current_a_callback},
		{"KERODHA", dh_a_callback},
		{"KERODHAV", dh_av_callback},
		{"KERODHB", dh_b_callback},
		{"KEROEPSR", eps_r_callback},
		{"KEROITERATIONS", iterations_callback},
		{"KEROKAPPA", kappa_callback},
		{"KEROKINTIME", kin_time_callback},
		{"KEROMU", mu_callback},
		{"KEROOSMOTIC", osmotic_callback},
		{"KEROPERCENTERROR", percent_error_callback},
		{"KEROPOTV", pot_v_callback},
		{"KEROPRESSURE", pressure_callback},
		{"KEROQBRN", qbrn_callback},
		{"KERORHO", rho_callback},
		{"KERORHOZERO", rho_0_callback},
		{"KEROSC", sc_callback},
		{"FZUNDERSCOREZVISC", f_visc_callback},
		{"MCDZUNDERSCOREZJTOT", mcd_jtot_callback},
		{"MCDZUNDERSCOREZJCONC", mcd_jconc_callback},
		{"SETDIFFZUNDERSCOREZC", setdiff_c_callback},
		{"GETZUNDERSCOREZPOR", get_por_callback},
		{"KEROCHANGEPOR", change_por_callback},
		{"ADDZUNDERSCOREZHEADING", add_heading_callback},
		{"KEROSOLNVOL", soln_vol_callback},
		{"KEROSIMTIME", sim_time_callback},
		{"KEROTOTALTIME", total_time_callback},
		{"KEROMID", mid_string_callback},
		{"KEROINSTR", instr_callback},
		{"KEROGRAPHX", graph_x_callback},
		{"KEROGRAPHY", graph_y_callback},
		{"KEROGRAPHSY", graph_sy_callback},
		{"KEROPLOTXY", plot_xy_callback},
		{"KERODATARESTORE", data_restore_callback},
		{"KERODATAREADNUMBER", data_read_number_callback},
		{"KERODATAREADSTRING", data_read_string_callback},
		{"KEROCHECKARRAY", array_budget_callback}
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

	std::string source = transform_source(commands, program);
	if (std::getenv("KEROTAKIS_TRACE_BASIC"))
	{
		std::fprintf(stderr, "--- transformed PHREEQC BASIC ---\n%s--- end transformed BASIC ---\n", source.c_str());
	}
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
		ArrayElements = 0;
		Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	}
	program->data_cursor = 0;
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
	if (result != MB_FUNC_OK && result != MB_FUNC_END)
	{
		if (LastError.empty())
		{
			std::ostringstream message;
			message << "MY-BASIC returned status " << result;
			LastError = message.str();
		}
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
			const char* text = value.value.string ? value.value.string : "";
			if (std::strcmp(text, KERO_NO_NEWLINE_MARKER) == 0) continue;
			std::vector<char> punch_text(text, text + std::strlen(text) + 1);
			if (std::strcmp(text, KERO_EOL_NOTAB_MARKER) == 0)
			{
				punch_text.assign(2, '\0');
				punch_text[0] = '\n';
				adapter->PhreeqcPtr->fpunchf_user(
					adapter->PhreeqcPtr->n_user_punch_index++, "%s", punch_text.data());
				continue;
			}
			adapter->PhreeqcPtr->fpunchf_user(
				adapter->PhreeqcPtr->n_user_punch_index++,
				"%s\t",
				punch_text.data());
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

int KeroBasicAdapter::line_print_callback(struct mb_interpreter_t* interpreter, void** local)
{
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	std::ostringstream output;
	output << std::setprecision(15);
	bool first = true;
	bool suppress_newline = false;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	while (mb_has_arg(interpreter, local))
	{
		mb_value_t value;
		mb_check(mb_pop_value(interpreter, local, &value));
		if (value.type == MB_DT_STRING)
		{
			const char* marker = value.value.string ? value.value.string : "";
			if (std::strcmp(marker, KERO_NO_NEWLINE_MARKER) == 0)
			{
				suppress_newline = true;
				continue;
			}
			if (std::strcmp(marker, KERO_EOL_NOTAB_MARKER) == 0)
			{
				output << '\n';
				first = true;
				continue;
			}
		}
		if (!first) output << '\t';
		if (value.type == MB_DT_INT)
			output << value.value.integer;
		else if (value.type == MB_DT_REAL)
			output << value.value.float_point;
		else if (value.type == MB_DT_STRING)
			output << (value.value.string ? value.value.string : "");
		else
		{
			adapter->LastError = "PRINT supports only numeric and string values";
			return MB_FUNC_ERR;
		}
		first = false;
	}
	mb_check(mb_attempt_close_bracket(interpreter, local));
	if (!suppress_newline) output << '\n';
	const std::string text = output.str();
	adapter->OutputBytes += text.size();
	if (adapter->OutputBytes > 1048576)
	{
		adapter->LastError = "output budget exceeded";
		return MB_FUNC_ERR;
	}
	adapter->PhreeqcPtr->output_msg(text.c_str());
	mb_check(mb_push_int(interpreter, local, 0));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::graph_x_callback(struct mb_interpreter_t* interpreter, void** local)
{
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
#if defined(MULTICHART)
	ChartObject* chart = adapter->PhreeqcPtr->chart_handler.Get_current_chart();
	if (!chart)
	{
		adapter->LastError = "GRAPH_X requires an active USER_GRAPH";
		return MB_FUNC_ERR;
	}
	mb_value_t value;
	LDBLE x = 0;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_pop_value(interpreter, local, &value));
	if (mb_has_arg(interpreter, local) || !numeric_value(value, x))
	{
		adapter->LastError = "GRAPH_X requires exactly one numeric value";
		return MB_FUNC_ERR;
	}
	mb_check(mb_attempt_close_bracket(interpreter, local));
	chart->Set_graph_x(x);
	mb_check(mb_push_real(interpreter, local, static_cast<real_t>(x)));
	return MB_FUNC_OK;
#else
	adapter->LastError = "GRAPH_X is unavailable without chart-data support";
	return MB_FUNC_ERR;
#endif
}

int KeroBasicAdapter::graph_y_common(struct mb_interpreter_t* interpreter, void** local, bool secondary)
{
	KeroBasicAdapter* adapter = KeroBasicAdapter::from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
#if defined(MULTICHART)
	ChartObject* chart = adapter->PhreeqcPtr->chart_handler.Get_current_chart();
	if (!chart)
	{
		adapter->LastError = secondary ? "GRAPH_SY requires an active USER_GRAPH" : "GRAPH_Y requires an active USER_GRAPH";
		return MB_FUNC_ERR;
	}
	mb_check(mb_attempt_open_bracket(interpreter, local));
	if (!mb_has_arg(interpreter, local))
	{
		adapter->LastError = secondary ? "GRAPH_SY requires a value" : "GRAPH_Y requires a value";
		return MB_FUNC_ERR;
	}
	while (mb_has_arg(interpreter, local))
	{
		mb_value_t value;
		LDBLE y = 0;
		mb_check(mb_pop_value(interpreter, local, &value));
		if (!numeric_value(value, y))
		{
			adapter->LastError = secondary ? "GRAPH_SY values must be numeric" : "GRAPH_Y values must be numeric";
			return MB_FUNC_ERR;
		}
		const int column = chart->Get_colnr();
		if (column < 0)
		{
			adapter->LastError = "USER_GRAPH column index is invalid";
			return MB_FUNC_ERR;
		}
		while (chart->Get_Curves().size() <= static_cast<size_t>(column))
		{
			const size_t heading_index = chart->Get_Curves().size() + 1;
			std::ostringstream fallback;
			fallback << "Series " << chart->Get_Curves().size() + 1;
			const std::string id = heading_index < chart->Get_new_headings().size()
				? chart->Get_new_headings()[heading_index]
				: fallback.str();
			chart->Add_curve(false, id, 1.0, "", 6.0, secondary ? 2 : 1, "");
			chart->Set_curve_added(true);
		}
		chart->Get_graph_y()[column] = y;
		if (secondary) chart->Get_secondary_y()[column] = true;
		chart->Set_colnr(column + 1);
	}
	mb_check(mb_attempt_close_bracket(interpreter, local));
	mb_check(mb_push_int(interpreter, local, 0));
	return MB_FUNC_OK;
#else
	adapter->LastError = secondary
		? "GRAPH_SY is unavailable without chart-data support"
		: "GRAPH_Y is unavailable without chart-data support";
	return MB_FUNC_ERR;
#endif
}

int KeroBasicAdapter::graph_y_callback(struct mb_interpreter_t* interpreter, void** local)
{
	return graph_y_common(interpreter, local, false);
}

int KeroBasicAdapter::graph_sy_callback(struct mb_interpreter_t* interpreter, void** local)
{
	return graph_y_common(interpreter, local, true);
}

int KeroBasicAdapter::plot_xy_callback(struct mb_interpreter_t* interpreter, void** local)
{
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
#if defined(MULTICHART)
	ChartObject* chart = adapter->PhreeqcPtr->chart_handler.Get_current_chart();
	if (!chart || !adapter->ActiveProgram)
	{
		adapter->LastError = "PLOT_XY requires an active USER_GRAPH";
		return MB_FUNC_ERR;
	}
	mb_value_t x_value;
	mb_value_t y_value;
	int_t style_index_value = 0;
	LDBLE x = 0;
	LDBLE y = 0;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_pop_int(interpreter, local, &style_index_value));
	mb_check(mb_pop_value(interpreter, local, &x_value));
	mb_check(mb_pop_value(interpreter, local, &y_value));
	if (mb_has_arg(interpreter, local) || !numeric_value(x_value, x) || !numeric_value(y_value, y))
	{
		adapter->LastError = "PLOT_XY requires exactly two numeric values";
		return MB_FUNC_ERR;
	}
	mb_check(mb_attempt_close_bracket(interpreter, local));

	const int column = chart->Get_colnr();
	if (column < 0)
	{
		adapter->LastError = "USER_GRAPH column index is invalid";
		return MB_FUNC_ERR;
	}
	while (chart->Get_Curves().size() <= static_cast<size_t>(column))
	{
		const size_t heading_index = chart->Get_Curves().size();
		std::ostringstream fallback;
		fallback << "Series " << heading_index + 1;
		const std::string id = heading_index < chart->Get_new_headings().size()
			? chart->Get_new_headings()[heading_index]
			: fallback.str();
		const size_t style_index = style_index_value < 0
			? chart->Get_new_plotxy_curves().size()
			: static_cast<size_t>(style_index_value);
		if (style_index < chart->Get_new_plotxy_curves().size())
		{
			CurveObject& style = chart->Get_new_plotxy_curves()[style_index];
			chart->Add_curve(
				true,
				id,
				style.Get_line_w(),
				style.Get_symbol(),
				style.Get_symbol_size(),
				style.Get_y_axis(),
				style.Get_color());
		}
		else
		{
			chart->Add_curve(true, id);
		}
		chart->Set_curve_added(true);
	}
	CurveObject* curve = chart->Get_Curves()[static_cast<size_t>(column)];
	curve->Get_x().push_back(x);
	curve->Get_y().push_back(y);
	chart->Set_point_added(true);
	chart->Set_colnr(column + 1);
	mb_check(mb_push_int(interpreter, local, 0));
	return MB_FUNC_OK;
#else
	adapter->LastError = "PLOT_XY is unavailable without chart-data support";
	return MB_FUNC_ERR;
#endif
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

int KeroBasicAdapter::get_callback(struct mb_interpreter_t* interpreter, void** local)
{
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	std::ostringstream key;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	while (mb_has_arg(interpreter, local))
	{
		mb_value_t value;
		LDBLE index = 0;
		mb_check(mb_pop_value(interpreter, local, &value));
		if (!numeric_value(value, index))
		{
			adapter->LastError = "GET indices must be numeric";
			return MB_FUNC_ERR;
		}
		key << static_cast<long long>(index) << ',';
	}
	mb_check(mb_attempt_close_bracket(interpreter, local));
	std::map<std::string, double>::const_iterator found = adapter->PhreeqcPtr->save_values.find(key.str());
	mb_check(mb_push_real(interpreter, local,
		static_cast<real_t>(found == adapter->PhreeqcPtr->save_values.end() ? 0.0 : found->second)));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::put_callback(struct mb_interpreter_t* interpreter, void** local)
{
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	mb_value_t stored;
	LDBLE number = 0;
	std::ostringstream key;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_pop_value(interpreter, local, &stored));
	if (!numeric_value(stored, number))
	{
		adapter->LastError = "PUT requires a numeric value";
		return MB_FUNC_ERR;
	}
	while (mb_has_arg(interpreter, local))
	{
		mb_value_t value;
		LDBLE index = 0;
		mb_check(mb_pop_value(interpreter, local, &value));
		if (!numeric_value(value, index))
		{
			adapter->LastError = "PUT indices must be numeric";
			return MB_FUNC_ERR;
		}
		key << static_cast<long long>(index) << ',';
	}
	mb_check(mb_attempt_close_bracket(interpreter, local));
	adapter->PhreeqcPtr->save_values[key.str()] = static_cast<double>(number);
	mb_check(mb_push_real(interpreter, local, static_cast<real_t>(number)));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::get_string_callback(struct mb_interpreter_t* interpreter, void** local)
{
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	std::ostringstream key;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	while (mb_has_arg(interpreter, local))
	{
		mb_value_t value;
		LDBLE index = 0;
		mb_check(mb_pop_value(interpreter, local, &value));
		if (!numeric_value(value, index)) return MB_FUNC_ERR;
		key << static_cast<long long>(index) << ',';
	}
	mb_check(mb_attempt_close_bracket(interpreter, local));
	std::map<std::string, std::string>::const_iterator found = adapter->PhreeqcPtr->save_strings.find(key.str());
	const std::string value = found == adapter->PhreeqcPtr->save_strings.end() ? "unknown" : found->second;
	mb_check(mb_push_string(interpreter, local,
		mb_memdup(value.c_str(), static_cast<unsigned>(value.size() + 1))));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::put_string_callback(struct mb_interpreter_t* interpreter, void** local)
{
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	char* stored = NULL;
	std::ostringstream key;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_pop_string(interpreter, local, &stored));
	if (!stored) return MB_FUNC_ERR;
	while (mb_has_arg(interpreter, local))
	{
		mb_value_t value;
		LDBLE index = 0;
		mb_check(mb_pop_value(interpreter, local, &value));
		if (!numeric_value(value, index)) return MB_FUNC_ERR;
		key << static_cast<long long>(index) << ',';
	}
	mb_check(mb_attempt_close_bracket(interpreter, local));
	adapter->PhreeqcPtr->save_strings[key.str()] = stored;
	mb_check(mb_push_string(interpreter, local,
		mb_memdup(stored, static_cast<unsigned>(std::strlen(stored) + 1))));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::exists_callback(struct mb_interpreter_t* interpreter, void** local)
{
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	std::ostringstream key;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	while (mb_has_arg(interpreter, local))
	{
		mb_value_t value;
		LDBLE index = 0;
		mb_check(mb_pop_value(interpreter, local, &value));
		if (!numeric_value(value, index))
		{
			adapter->LastError = "EXISTS indices must be numeric";
			return MB_FUNC_ERR;
		}
		key << static_cast<long long>(index) << ',';
	}
	mb_check(mb_attempt_close_bracket(interpreter, local));
	mb_check(mb_push_int(interpreter, local,
		adapter->PhreeqcPtr->save_values.find(key.str()) == adapter->PhreeqcPtr->save_values.end() ? 0 : 1));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::eol_callback(struct mb_interpreter_t* interpreter, void** local)
{
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	mb_check(mb_push_string(interpreter, local, mb_memdup("\n", 2)));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::eol_notab_callback(struct mb_interpreter_t* interpreter, void** local)
{
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	mb_check(mb_push_string(interpreter, local,
		mb_memdup(KERO_EOL_NOTAB_MARKER, static_cast<unsigned>(sizeof(KERO_EOL_NOTAB_MARKER)))));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::no_newline_callback(struct mb_interpreter_t* interpreter, void** local)
{
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	mb_check(mb_push_string(interpreter, local,
		mb_memdup(KERO_NO_NEWLINE_MARKER, static_cast<unsigned>(sizeof(KERO_NO_NEWLINE_MARKER)))));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::str_callback(struct mb_interpreter_t* interpreter, void** local)
{
	mb_value_t number_value;
	LDBLE number = 0;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_pop_value(interpreter, local, &number_value));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	if (!numeric_value(number_value, number)) return MB_FUNC_ERR;
	std::ostringstream formatted;
	formatted.imbue(std::locale::classic());
	if (number_value.type == MB_DT_INT)
	{
		formatted << number_value.value.integer;
	}
	else
	{
		formatted << std::defaultfloat << std::setprecision(6) << static_cast<double>(number);
	}
	std::string value = formatted.str();
	if (number >= 0) value.insert(value.begin(), ' ');
	mb_check(mb_push_string(interpreter, local,
		mb_memdup(value.c_str(), static_cast<unsigned>(value.size() + 1))));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::str_e_callback(struct mb_interpreter_t* interpreter, void** local)
{
	mb_value_t number_value;
	LDBLE number = 0;
	int_t length = 0;
	int_t precision = 0;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_pop_value(interpreter, local, &number_value));
	mb_check(mb_pop_int(interpreter, local, &length));
	mb_check(mb_pop_int(interpreter, local, &precision));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	if (!numeric_value(number_value, number) || length < 0 || precision < 0) return MB_FUNC_ERR;
	std::ostringstream formatted;
	formatted << std::scientific << std::setw(static_cast<int>(length))
		<< std::setprecision(static_cast<int>(precision)) << static_cast<double>(number);
	const std::string value = formatted.str();
	mb_check(mb_push_string(interpreter, local,
		mb_memdup(value.c_str(), static_cast<unsigned>(value.size() + 1))));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::str_f_callback(struct mb_interpreter_t* interpreter, void** local)
{
	mb_value_t number_value;
	LDBLE number = 0;
	int_t length = 0;
	int_t precision = 0;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_pop_value(interpreter, local, &number_value));
	mb_check(mb_pop_int(interpreter, local, &length));
	mb_check(mb_pop_int(interpreter, local, &precision));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	if (!numeric_value(number_value, number) || length < 0 || precision < 0) return MB_FUNC_ERR;
	std::ostringstream formatted;
	formatted << std::fixed << std::setw(static_cast<int>(length))
		<< std::setprecision(static_cast<int>(precision)) << static_cast<double>(number);
	const std::string value = formatted.str();
	mb_check(mb_push_string(interpreter, local,
		mb_memdup(value.c_str(), static_cast<unsigned>(value.size() + 1))));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::pad_callback(struct mb_interpreter_t* interpreter, void** local)
{
	char* text = NULL;
	int_t width = 0;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_pop_string(interpreter, local, &text));
	mb_check(mb_pop_int(interpreter, local, &width));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	if (!text || width < 0) return MB_FUNC_ERR;
	std::string padded(text);
	if (padded.size() < static_cast<size_t>(width)) padded.append(static_cast<size_t>(width) - padded.size(), ' ');
	mb_check(mb_push_string(interpreter, local,
		mb_memdup(padded.c_str(), static_cast<unsigned>(padded.size() + 1))));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::trim_callback(struct mb_interpreter_t* interpreter, void** local)
{
	char* text = NULL;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_pop_string(interpreter, local, &text));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	if (!text) return MB_FUNC_ERR;
	std::string value(text);
	const std::string::size_type first = value.find_first_not_of(" \t\r\n");
	if (first == std::string::npos) value.clear();
	else value = value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
	mb_check(mb_push_string(interpreter, local,
		mb_memdup(value.c_str(), static_cast<unsigned>(value.size() + 1))));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::ltrim_callback(struct mb_interpreter_t* interpreter, void** local)
{
	char* text = NULL;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_pop_string(interpreter, local, &text));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	if (!text) return MB_FUNC_ERR;
	std::string value(text);
	const std::string::size_type first = value.find_first_not_of(" \t\r\n");
	value = first == std::string::npos ? std::string() : value.substr(first);
	mb_check(mb_push_string(interpreter, local,
		mb_memdup(value.c_str(), static_cast<unsigned>(value.size() + 1))));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::rtrim_callback(struct mb_interpreter_t* interpreter, void** local)
{
	char* text = NULL;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_pop_string(interpreter, local, &text));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	if (!text) return MB_FUNC_ERR;
	std::string value(text);
	const std::string::size_type last = value.find_last_not_of(" \t\r\n");
	value = last == std::string::npos ? std::string() : value.substr(0, last + 1);
	mb_check(mb_push_string(interpreter, local,
		mb_memdup(value.c_str(), static_cast<unsigned>(value.size() + 1))));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::iso_unit_callback(struct mb_interpreter_t* interpreter, void** local)
{
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	char* name = NULL;
	if (!adapter) return MB_FUNC_ERR;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_pop_string(interpreter, local, &name));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	if (!name) return MB_FUNC_ERR;
	char* unit = adapter->PhreeqcPtr->iso_unit(name);
	const std::string value = unit ? unit : "unknown";
	unit = static_cast<char*>(adapter->PhreeqcPtr->free_check_null(unit));
	mb_check(mb_push_string(interpreter, local,
		mb_memdup(value.c_str(), static_cast<unsigned>(value.size() + 1))));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::title_callback(struct mb_interpreter_t* interpreter, void** local)
{
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	std::string value = adapter->PhreeqcPtr->last_title_x.empty() ? " " : adapter->PhreeqcPtr->last_title_x;
	std::replace(value.begin(), value.end(), '\t', ' ');
	mb_check(mb_push_string(interpreter, local,
		mb_memdup(value.c_str(), static_cast<unsigned>(value.size() + 1))));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::description_callback(struct mb_interpreter_t* interpreter, void** local)
{
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	std::string value = "Unknown";
	if (adapter->PhreeqcPtr->state == REACTION)
	{
		if (adapter->PhreeqcPtr->use.Get_mix_in())
		{
			std::ostringstream description;
			description << "Mix " << adapter->PhreeqcPtr->use.Get_n_mix_user();
			value = description.str();
		}
		else
		{
			const int number = adapter->PhreeqcPtr->use.Get_n_solution_user();
			std::map<int, cxxSolution>::const_iterator solution =
				adapter->PhreeqcPtr->Rxn_solution_map.find(number);
			if (solution != adapter->PhreeqcPtr->Rxn_solution_map.end())
				value = solution->second.Get_description();
		}
	}
	else if (adapter->PhreeqcPtr->state == ADVECTION ||
		adapter->PhreeqcPtr->state == TRANSPORT || adapter->PhreeqcPtr->state == PHAST)
	{
		std::ostringstream description;
		description << "Cell " << adapter->PhreeqcPtr->cell_no;
		value = description.str();
	}
	else if (adapter->PhreeqcPtr->use.Get_solution_ptr())
	{
		value = adapter->PhreeqcPtr->use.Get_solution_ptr()->Get_description();
	}
	mb_check(mb_push_string(interpreter, local,
		mb_memdup(value.c_str(), static_cast<unsigned>(value.size() + 1))));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::set_count_and_arrays(
	struct mb_interpreter_t* interpreter,
	void** local,
	void* count_var,
	void* names_var,
	void* values_var,
	const std::vector<std::string>& names,
	const std::vector<double>& values)
{
	if (!count_var || !names_var || !values_var || names.size() != values.size()) return MB_FUNC_ERR;
	mb_value_t count_value;
	mb_make_int(count_value, static_cast<int_t>(names.size()));
	mb_check(mb_set_var_value(interpreter, count_var, count_value));
	int dimension = static_cast<int>(names.size()) + 1;
	void* names_array = NULL;
	void* values_array = NULL;
	mb_check(mb_init_array(interpreter, local, MB_DT_STRING, &dimension, 1, &names_array));
	mb_check(mb_init_array(interpreter, local, MB_DT_REAL, &dimension, 1, &values_array));
	mb_value_t names_value;
	mb_value_t values_value;
	mb_make_array(names_value, names_array);
	mb_make_array(values_value, values_array);
	mb_check(mb_set_var_value(interpreter, names_var, names_value));
	mb_check(mb_set_var_value(interpreter, values_var, values_value));
	for (size_t item = 0; item < names.size(); ++item)
	{
		int index = static_cast<int>(item) + 1;
		mb_value_t name_value;
		mb_value_t number_value;
		mb_make_string(name_value,
			mb_memdup(names[item].c_str(), static_cast<unsigned>(names[item].size() + 1)));
		mb_make_real(number_value, static_cast<real_t>(values[item]));
		mb_check(mb_set_array_elem(interpreter, local, names_array, &index, 1, name_value));
		mb_check(mb_set_array_elem(interpreter, local, values_array, &index, 1, number_value));
	}
	return MB_FUNC_OK;
}

int KeroBasicAdapter::list_ss_callback(struct mb_interpreter_t* interpreter, void** local)
{
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	char* name = NULL;
	void* count_var = NULL;
	void* names_var = NULL;
	void* moles_var = NULL;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_pop_string(interpreter, local, &name));
	mb_check(mb_get_var(interpreter, local, &count_var, true));
	mb_check(mb_get_var(interpreter, local, &names_var, true));
	mb_check(mb_get_var(interpreter, local, &moles_var, true));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	if (!name || !count_var || !names_var || !moles_var)
	{
		adapter->LastError = "LIST_S_S requires name, count, names$, and moles variables";
		return MB_FUNC_ERR;
	}
	cxxNameDouble composition;
	const LDBLE total = adapter->PhreeqcPtr->list_ss(name, composition);
	mb_value_t count_value;
	mb_make_int(count_value, static_cast<int_t>(composition.size()));
	mb_check(mb_set_var_value(interpreter, count_var, count_value));

	int dimension = static_cast<int>(composition.size()) + 1;
	void* names_array = NULL;
	void* moles_array = NULL;
	mb_check(mb_init_array(interpreter, local, MB_DT_STRING, &dimension, 1, &names_array));
	mb_check(mb_init_array(interpreter, local, MB_DT_REAL, &dimension, 1, &moles_array));
	mb_value_t names_value;
	mb_value_t moles_value;
	mb_make_array(names_value, names_array);
	mb_make_array(moles_value, moles_array);
	mb_check(mb_set_var_value(interpreter, names_var, names_value));
	mb_check(mb_set_var_value(interpreter, moles_var, moles_value));
	int index = 1;
	for (cxxNameDouble::const_iterator it = composition.begin(); it != composition.end(); ++it, ++index)
	{
		mb_value_t item_name;
		mb_value_t item_moles;
		mb_make_string(item_name, mb_memdup(it->first.c_str(), static_cast<unsigned>(it->first.size() + 1)));
		mb_make_real(item_moles, static_cast<real_t>(it->second));
		mb_check(mb_set_array_elem(interpreter, local, names_array, &index, 1, item_name));
		mb_check(mb_set_array_elem(interpreter, local, moles_array, &index, 1, item_moles));
	}
	mb_check(mb_push_real(interpreter, local, static_cast<real_t>(total)));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::edl_species_callback(struct mb_interpreter_t* interpreter, void** local)
{
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	char* surface = NULL;
	void* count_var = NULL;
	void* names_var = NULL;
	void* moles_var = NULL;
	void* area_var = NULL;
	void* thickness_var = NULL;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_pop_string(interpreter, local, &surface));
	mb_check(mb_get_var(interpreter, local, &count_var, true));
	mb_check(mb_get_var(interpreter, local, &names_var, true));
	mb_check(mb_get_var(interpreter, local, &moles_var, true));
	mb_check(mb_get_var(interpreter, local, &area_var, true));
	mb_check(mb_get_var(interpreter, local, &thickness_var, true));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	if (!surface || !count_var || !names_var || !moles_var || !area_var || !thickness_var)
	{
		adapter->LastError = "EDL_SPECIES requires surface, count, names$, moles, area, and thickness variables";
		return MB_FUNC_ERR;
	}
	LDBLE count = 0;
	LDBLE area = 0;
	LDBLE thickness = 0;
	char** raw_names = NULL;
	LDBLE* raw_moles = NULL;
	const LDBLE total = adapter->PhreeqcPtr->edl_species(
		surface, &count, &raw_names, &raw_moles, &area, &thickness);
	std::vector<std::string> names;
	std::vector<double> moles;
	for (int i = 1; i <= static_cast<int>(count); ++i)
	{
		names.push_back(raw_names && raw_names[i] ? raw_names[i] : "");
		moles.push_back(raw_moles ? raw_moles[i] : 0);
		if (raw_names) raw_names[i] = static_cast<char*>(adapter->PhreeqcPtr->free_check_null(raw_names[i]));
	}
	adapter->PhreeqcPtr->free_check_null(raw_names);
	adapter->PhreeqcPtr->free_check_null(raw_moles);
	mb_check(set_count_and_arrays(interpreter, local, count_var, names_var, moles_var, names, moles));
	mb_value_t area_value;
	mb_value_t thickness_value;
	mb_make_real(area_value, static_cast<real_t>(area));
	mb_make_real(thickness_value, static_cast<real_t>(thickness));
	mb_check(mb_set_var_value(interpreter, area_var, area_value));
	mb_check(mb_set_var_value(interpreter, thickness_var, thickness_value));
	mb_check(mb_push_real(interpreter, local, static_cast<real_t>(total)));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::system_total_callback(struct mb_interpreter_t* interpreter, void** local)
{
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	char* total_name = NULL;
	void* count_var = NULL;
	void* names_var = NULL;
	void* types_var = NULL;
	void* moles_var = NULL;
	int_t sort = 0;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_pop_string(interpreter, local, &total_name));
	const bool has_outputs = mb_has_arg(interpreter, local) != 0;
	if (has_outputs)
	{
		mb_check(mb_get_var(interpreter, local, &count_var, true));
		mb_check(mb_get_var(interpreter, local, &names_var, true));
		mb_check(mb_get_var(interpreter, local, &types_var, true));
		mb_check(mb_get_var(interpreter, local, &moles_var, true));
		if (mb_has_arg(interpreter, local)) mb_check(mb_pop_int(interpreter, local, &sort));
	}
	mb_check(mb_attempt_close_bracket(interpreter, local));
	if (!total_name || (has_outputs && (!count_var || !names_var || !types_var || !moles_var)))
	{
		adapter->LastError = "SYS requires a total name and optional count, names$, types$, and moles variables";
		return MB_FUNC_ERR;
	}
	LDBLE count = 0;
	char** raw_names = NULL;
	char** raw_types = NULL;
	LDBLE* raw_moles = NULL;
	const LDBLE total = adapter->PhreeqcPtr->system_total(
		total_name, &count, &raw_names, &raw_types, &raw_moles, static_cast<int>(sort));
	std::vector<std::string> names;
	std::vector<std::string> types;
	std::vector<double> moles;
	for (int i = 1; i <= static_cast<int>(count); ++i)
	{
		names.push_back(raw_names && raw_names[i] ? raw_names[i] : "");
		types.push_back(raw_types && raw_types[i] ? raw_types[i] : "");
		moles.push_back(raw_moles ? raw_moles[i] : 0);
		if (raw_names) raw_names[i] = static_cast<char*>(adapter->PhreeqcPtr->free_check_null(raw_names[i]));
		if (raw_types) raw_types[i] = static_cast<char*>(adapter->PhreeqcPtr->free_check_null(raw_types[i]));
	}
	adapter->PhreeqcPtr->free_check_null(raw_names);
	adapter->PhreeqcPtr->free_check_null(raw_types);
	adapter->PhreeqcPtr->free_check_null(raw_moles);
	if (has_outputs)
	{
		mb_check(set_count_and_arrays(interpreter, local, count_var, names_var, moles_var, names, moles));
		int dimension = static_cast<int>(types.size()) + 1;
		void* types_array = NULL;
		mb_check(mb_init_array(interpreter, local, MB_DT_STRING, &dimension, 1, &types_array));
		mb_value_t types_value;
		mb_make_array(types_value, types_array);
		mb_check(mb_set_var_value(interpreter, types_var, types_value));
		for (size_t item = 0; item < types.size(); ++item)
		{
			int index = static_cast<int>(item) + 1;
			mb_value_t type_value;
			mb_make_string(type_value,
				mb_memdup(types[item].c_str(), static_cast<unsigned>(types[item].size() + 1)));
			mb_check(mb_set_array_elem(interpreter, local, types_array, &index, 1, type_value));
		}
	}
	mb_check(mb_push_real(interpreter, local, static_cast<real_t>(total)));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::equivalent_fraction_callback(struct mb_interpreter_t* interpreter, void** local)
{
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	char* species = NULL;
	void* equivalents_var = NULL;
	void* element_var = NULL;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_pop_string(interpreter, local, &species));
	mb_check(mb_get_var(interpreter, local, &equivalents_var, true));
	mb_check(mb_get_var(interpreter, local, &element_var, true));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	if (!species || !equivalents_var || !element_var) return MB_FUNC_ERR;
	LDBLE equivalents = 0;
	std::string element;
	const LDBLE fraction = adapter->PhreeqcPtr->equivalent_fraction(species, &equivalents, element);
	mb_value_t equivalents_value;
	mb_value_t element_value;
	mb_make_real(equivalents_value, static_cast<real_t>(equivalents));
	mb_make_string(element_value,
		mb_memdup(element.c_str(), static_cast<unsigned>(element.size() + 1)));
	mb_check(mb_set_var_value(interpreter, equivalents_var, equivalents_value));
	mb_check(mb_set_var_value(interpreter, element_var, element_value));
	mb_check(mb_push_real(interpreter, local, static_cast<real_t>(fraction)));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::mean_gamma_callback(struct mb_interpreter_t* interpreter, void** local)
{
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	char* name = NULL;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_pop_string(interpreter, local, &name));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	if (!name) return MB_FUNC_ERR;
	std::string key(name);
	std::transform(key.begin(), key.end(), key.begin(),
		static_cast<int (*)(int)>(std::tolower));
	std::map<std::string, cxxNameDouble>::const_iterator definition =
		adapter->PhreeqcPtr->mean_gammas.find(key);
	if (definition == adapter->PhreeqcPtr->mean_gammas.end() || definition->second.empty())
	{
		adapter->LastError = std::string("No definition in MEAN_GAMMAS found for ") + name;
		return MB_FUNC_ERR;
	}
	LDBLE product = 1;
	LDBLE exponent_sum = 0;
	for (cxxNameDouble::const_iterator item = definition->second.begin(); item != definition->second.end(); ++item)
	{
		product *= std::pow(adapter->PhreeqcPtr->activity_coefficient(item->first.c_str()), item->second);
		exponent_sum += item->second;
	}
	if (exponent_sum == 0) return MB_FUNC_ERR;
	mb_check(mb_push_real(interpreter, local,
		static_cast<real_t>(std::pow(product, 1.0 / exponent_sum))));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::debye_length_callback(struct mb_interpreter_t* interpreter, void** local)
{
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	LDBLE value = 0;
	if (adapter->PhreeqcPtr->mu_x > 0)
	{
		const LDBLE square =
			(adapter->PhreeqcPtr->eps_r * EPSILON_ZERO * R_KJ_DEG_MOL * 1000.0 * adapter->PhreeqcPtr->tk_x) /
			(2.0 * F_C_MOL * F_C_MOL * adapter->PhreeqcPtr->mu_x * 1000.0);
		value = square > 0 ? std::sqrt(square) : 0;
	}
	mb_check(mb_push_real(interpreter, local, static_cast<real_t>(value)));
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

#define KERO_NAMED_CALLBACK(name, kind) \
	int KeroBasicAdapter::name(struct mb_interpreter_t* interpreter, void** local) \
	{ return named_chemistry_callback(interpreter, local, kind); }
KERO_NAMED_CALLBACK(phase_delta_h_callback, VALUE_PHASE_DELTA_H)
KERO_NAMED_CALLBACK(dh_a0_callback, VALUE_DH_A0)
KERO_NAMED_CALLBACK(dh_bdot_callback, VALUE_DH_BDOT)
KERO_NAMED_CALLBACK(diff_c_callback, VALUE_DIFF_C)
KERO_NAMED_CALLBACK(equi_delta_callback, VALUE_EQUI_DELTA)
KERO_NAMED_CALLBACK(gamma_callback, VALUE_GAMMA)
KERO_NAMED_CALLBACK(kin_delta_callback, VALUE_KIN_DELTA)
KERO_NAMED_CALLBACK(log_gamma_callback, VALUE_LOG_GAMMA)
KERO_NAMED_CALLBACK(lk_named_callback, VALUE_LK_NAMED)
KERO_NAMED_CALLBACK(lk_phase_callback, VALUE_LK_PHASE)
KERO_NAMED_CALLBACK(lk_species_callback, VALUE_LK_SPECIES)
KERO_NAMED_CALLBACK(phase_vm_callback, VALUE_PHASE_VM)
KERO_NAMED_CALLBACK(pr_phi_callback, VALUE_PR_PHI)
KERO_NAMED_CALLBACK(t_sc_callback, VALUE_T_SC)
#undef KERO_NAMED_CALLBACK

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
	void* count_var = NULL;
	void* names_var = NULL;
	void* coefficients_var = NULL;
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_pop_string(interpreter, local, &name));
	const bool has_outputs = mb_has_arg(interpreter, local) != 0;
	if (has_outputs)
	{
		mb_check(mb_get_var(interpreter, local, &count_var, true));
		mb_check(mb_get_var(interpreter, local, &names_var, true));
		mb_check(mb_get_var(interpreter, local, &coefficients_var, true));
	}
	mb_check(mb_attempt_close_bracket(interpreter, local));
	if (!name || (has_outputs && (!count_var || !names_var || !coefficients_var)))
	{
		adapter->LastError = "PHASE_FORMULA requires a phase name and optional count, names$, and coefficients variables";
		return MB_FUNC_ERR;
	}
	cxxNameDouble stoichiometry;
	std::string formula = adapter->PhreeqcPtr->phase_formula(name ? name : "", stoichiometry);
	if (has_outputs)
	{
		std::vector<std::string> names;
		std::vector<double> coefficients;
		for (cxxNameDouble::const_iterator item = stoichiometry.begin(); item != stoichiometry.end(); ++item)
		{
			names.push_back(item->first);
			coefficients.push_back(item->second);
		}
		mb_check(set_count_and_arrays(interpreter, local,
			count_var, names_var, coefficients_var, names, coefficients));
	}
	mb_check(mb_push_string(interpreter, local, mb_memdup(formula.c_str(), static_cast<unsigned>(formula.size() + 1))));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::kinetics_formula_callback(struct mb_interpreter_t* interpreter, void** local)
{
	char* name = NULL;
	void* count_var = NULL;
	void* names_var = NULL;
	void* coefficients_var = NULL;
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_pop_string(interpreter, local, &name));
	const bool has_outputs = mb_has_arg(interpreter, local) != 0;
	if (has_outputs)
	{
		mb_check(mb_get_var(interpreter, local, &count_var, true));
		mb_check(mb_get_var(interpreter, local, &names_var, true));
		mb_check(mb_get_var(interpreter, local, &coefficients_var, true));
	}
	mb_check(mb_attempt_close_bracket(interpreter, local));
	if (!name || (has_outputs && (!count_var || !names_var || !coefficients_var))) return MB_FUNC_ERR;
	cxxNameDouble stoichiometry;
	const std::string formula = adapter->PhreeqcPtr->kinetics_formula(name, stoichiometry);
	if (has_outputs)
	{
		std::vector<std::string> names;
		std::vector<double> coefficients;
		for (cxxNameDouble::const_iterator item = stoichiometry.begin(); item != stoichiometry.end(); ++item)
		{
			names.push_back(item->first);
			coefficients.push_back(item->second);
		}
		mb_check(set_count_and_arrays(interpreter, local,
			count_var, names_var, coefficients_var, names, coefficients));
	}
	mb_check(mb_push_string(interpreter, local,
		mb_memdup(formula.c_str(), static_cast<unsigned>(formula.size() + 1))));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::species_formula_callback(struct mb_interpreter_t* interpreter, void** local)
{
	char* name = NULL;
	void* count_var = NULL;
	void* names_var = NULL;
	void* coefficients_var = NULL;
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_pop_string(interpreter, local, &name));
	mb_check(mb_get_var(interpreter, local, &count_var, true));
	mb_check(mb_get_var(interpreter, local, &names_var, true));
	mb_check(mb_get_var(interpreter, local, &coefficients_var, true));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	if (!name || !count_var || !names_var || !coefficients_var) return MB_FUNC_ERR;
	cxxNameDouble stoichiometry;
	const std::string type = adapter->PhreeqcPtr->species_formula(name, stoichiometry);
	std::vector<std::string> names;
	std::vector<double> coefficients;
	for (cxxNameDouble::const_iterator item = stoichiometry.begin(); item != stoichiometry.end(); ++item)
	{
		names.push_back(item->first);
		coefficients.push_back(item->second);
	}
	mb_check(set_count_and_arrays(interpreter, local,
		count_var, names_var, coefficients_var, names, coefficients));
	mb_check(mb_push_string(interpreter, local,
		mb_memdup(type.c_str(), static_cast<unsigned>(type.size() + 1))));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::phase_equation_callback(struct mb_interpreter_t* interpreter, void** local)
{
	char* name = NULL;
	void* count_var = NULL;
	void* names_var = NULL;
	void* coefficients_var = NULL;
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_pop_string(interpreter, local, &name));
	mb_check(mb_get_var(interpreter, local, &count_var, true));
	mb_check(mb_get_var(interpreter, local, &names_var, true));
	mb_check(mb_get_var(interpreter, local, &coefficients_var, true));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	if (!name || !count_var || !names_var || !coefficients_var) return MB_FUNC_ERR;
	std::vector<std::pair<std::string, double> > stoichiometry;
	const std::string equation = adapter->PhreeqcPtr->phase_equation(name, stoichiometry);
	std::vector<std::string> names;
	std::vector<double> coefficients;
	for (std::vector<std::pair<std::string, double> >::const_iterator item = stoichiometry.begin();
		item != stoichiometry.end(); ++item)
	{
		names.push_back(item->first);
		coefficients.push_back(item->second);
	}
	mb_check(set_count_and_arrays(interpreter, local,
		count_var, names_var, coefficients_var, names, coefficients));
	mb_check(mb_push_string(interpreter, local,
		mb_memdup(equation.c_str(), static_cast<unsigned>(equation.size() + 1))));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::species_equation_callback(struct mb_interpreter_t* interpreter, void** local)
{
	char* name = NULL;
	void* count_var = NULL;
	void* names_var = NULL;
	void* coefficients_var = NULL;
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_pop_string(interpreter, local, &name));
	mb_check(mb_get_var(interpreter, local, &count_var, true));
	mb_check(mb_get_var(interpreter, local, &names_var, true));
	mb_check(mb_get_var(interpreter, local, &coefficients_var, true));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	if (!name || !count_var || !names_var || !coefficients_var) return MB_FUNC_ERR;
	std::vector<std::pair<std::string, double> > stoichiometry;
	const std::string equation = adapter->PhreeqcPtr->species_equation(name, stoichiometry);
	std::vector<std::string> names;
	std::vector<double> coefficients;
	for (std::vector<std::pair<std::string, double> >::const_iterator item = stoichiometry.begin();
		item != stoichiometry.end(); ++item)
	{
		names.push_back(item->first);
		coefficients.push_back(item->second);
	}
	mb_check(set_count_and_arrays(interpreter, local,
		count_var, names_var, coefficients_var, names, coefficients));
	mb_check(mb_push_string(interpreter, local,
		mb_memdup(equation.c_str(), static_cast<unsigned>(equation.size() + 1))));
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

int KeroBasicAdapter::sum_ss_callback(struct mb_interpreter_t* interpreter, void** local)
{
	char* pattern = NULL;
	char* element = NULL;
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_pop_string(interpreter, local, &pattern));
	if (mb_has_arg(interpreter, local)) mb_check(mb_pop_string(interpreter, local, &element));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	if (!pattern) { adapter->LastError = "SUM_S_S requires a template"; return MB_FUNC_ERR; }
	mb_check(mb_push_real(interpreter, local,
		static_cast<real_t>(adapter->PhreeqcPtr->sum_match_ss(pattern, element))));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::surface_callback(struct mb_interpreter_t* interpreter, void** local)
{
	char* total = NULL;
	char* surface = NULL;
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_pop_string(interpreter, local, &total));
	if (mb_has_arg(interpreter, local)) mb_check(mb_pop_string(interpreter, local, &surface));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	if (!total) { adapter->LastError = "SURF requires a total name"; return MB_FUNC_ERR; }
	mb_check(mb_push_real(interpreter, local,
		static_cast<real_t>(adapter->PhreeqcPtr->surf_total(total, surface))));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::edl_callback(struct mb_interpreter_t* interpreter, void** local)
{
	char* total = NULL;
	char* surface = NULL;
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_pop_string(interpreter, local, &total));
	if (mb_has_arg(interpreter, local)) mb_check(mb_pop_string(interpreter, local, &surface));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	if (!total) { adapter->LastError = "EDL requires a total name"; return MB_FUNC_ERR; }
	mb_check(mb_push_real(interpreter, local,
		static_cast<real_t>(adapter->PhreeqcPtr->diff_layer_total(total, surface))));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::misc1_callback(struct mb_interpreter_t* interpreter, void** local)
{
	char* name = NULL;
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_pop_string(interpreter, local, &name));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	if (!name) return MB_FUNC_ERR;
	mb_check(mb_push_real(interpreter, local, static_cast<real_t>(adapter->PhreeqcPtr->find_misc1(name))));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::misc2_callback(struct mb_interpreter_t* interpreter, void** local)
{
	char* name = NULL;
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_pop_string(interpreter, local, &name));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	if (!name) return MB_FUNC_ERR;
	mb_check(mb_push_real(interpreter, local, static_cast<real_t>(adapter->PhreeqcPtr->find_misc2(name))));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::isotope_callback(struct mb_interpreter_t* interpreter, void** local)
{
	char* name = NULL;
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_pop_string(interpreter, local, &name));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	if (!name) return MB_FUNC_ERR;
	mb_check(mb_push_real(interpreter, local, static_cast<real_t>(adapter->PhreeqcPtr->iso_value(name))));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::partial_pressure_callback(struct mb_interpreter_t* interpreter, void** local)
{
	char* name = NULL;
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_pop_string(interpreter, local, &name));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	if (!name) return MB_FUNC_ERR;
	mb_check(mb_push_real(interpreter, local, static_cast<real_t>(adapter->PhreeqcPtr->pr_pressure(name))));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::gas_pressure_callback(struct mb_interpreter_t* interpreter, void** local)
{
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	mb_check(mb_push_real(interpreter, local, static_cast<real_t>(adapter->PhreeqcPtr->find_gas_p())));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::gas_molar_volume_callback(struct mb_interpreter_t* interpreter, void** local)
{
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	mb_check(mb_push_real(interpreter, local, static_cast<real_t>(adapter->PhreeqcPtr->find_gas_vm())));
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

int KeroBasicAdapter::step_no_callback(struct mb_interpreter_t* interpreter, void** local)
{
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	LDBLE value = 0;
	if (adapter->PhreeqcPtr->state == TRANSPORT) value = adapter->PhreeqcPtr->transport_step;
	else if (adapter->PhreeqcPtr->state == ADVECTION) value = adapter->PhreeqcPtr->advection_step;
	else if (adapter->PhreeqcPtr->state == REACTION) value = adapter->PhreeqcPtr->reaction_step;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	mb_check(mb_push_real(interpreter, local, static_cast<real_t>(value)));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::sim_no_callback(struct mb_interpreter_t* interpreter, void** local)
{
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	mb_check(mb_push_real(interpreter, local, static_cast<real_t>(adapter->PhreeqcPtr->simulation)));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::distance_callback(struct mb_interpreter_t* interpreter, void** local)
{
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	LDBLE value = 0;
	if (adapter->PhreeqcPtr->state == TRANSPORT && adapter->PhreeqcPtr->cell >= 0 &&
		static_cast<size_t>(adapter->PhreeqcPtr->cell) < adapter->PhreeqcPtr->cell_data.size())
		value = adapter->PhreeqcPtr->cell_data[static_cast<size_t>(adapter->PhreeqcPtr->cell)].mid_cell_x;
	else if (adapter->PhreeqcPtr->state == ADVECTION)
		value = adapter->PhreeqcPtr->use.Get_n_solution_user();
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	mb_check(mb_push_real(interpreter, local, static_cast<real_t>(value)));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::reaction_increment_callback(struct mb_interpreter_t* interpreter, void** local)
{
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	const LDBLE value = adapter->PhreeqcPtr->state == REACTION || adapter->PhreeqcPtr->state == ADVECTION ||
		adapter->PhreeqcPtr->state == TRANSPORT ? adapter->PhreeqcPtr->step_x : 0;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	mb_check(mb_push_real(interpreter, local, static_cast<real_t>(value)));
	return MB_FUNC_OK;
}

#define KERO_SCALAR_CALLBACK(name, kind) \
	int KeroBasicAdapter::name(struct mb_interpreter_t* interpreter, void** local) \
	{ return scalar_value_callback(interpreter, local, kind); }
KERO_SCALAR_CALLBACK(alk_callback, SCALAR_ALK)
KERO_SCALAR_CALLBACK(aphi_callback, SCALAR_APHI)
KERO_SCALAR_CALLBACK(charge_balance_callback, SCALAR_CHARGE_BALANCE)
KERO_SCALAR_CALLBACK(current_a_callback, SCALAR_CURRENT_A)
KERO_SCALAR_CALLBACK(dh_a_callback, SCALAR_DH_A)
KERO_SCALAR_CALLBACK(dh_av_callback, SCALAR_DH_AV)
KERO_SCALAR_CALLBACK(dh_b_callback, SCALAR_DH_B)
KERO_SCALAR_CALLBACK(eps_r_callback, SCALAR_EPS_R)
KERO_SCALAR_CALLBACK(iterations_callback, SCALAR_ITERATIONS)
KERO_SCALAR_CALLBACK(kappa_callback, SCALAR_KAPPA)
KERO_SCALAR_CALLBACK(kin_time_callback, SCALAR_KIN_TIME)
KERO_SCALAR_CALLBACK(mu_callback, SCALAR_MU)
KERO_SCALAR_CALLBACK(osmotic_callback, SCALAR_OSMOTIC)
KERO_SCALAR_CALLBACK(percent_error_callback, SCALAR_PERCENT_ERROR)
KERO_SCALAR_CALLBACK(pot_v_callback, SCALAR_POT_V)
KERO_SCALAR_CALLBACK(pressure_callback, SCALAR_PRESSURE)
KERO_SCALAR_CALLBACK(qbrn_callback, SCALAR_QBRN)
KERO_SCALAR_CALLBACK(rho_callback, SCALAR_RHO)
KERO_SCALAR_CALLBACK(rho_0_callback, SCALAR_RHO_0)
KERO_SCALAR_CALLBACK(sc_callback, SCALAR_SC)
#undef KERO_SCALAR_CALLBACK

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

int KeroBasicAdapter::instr_callback(struct mb_interpreter_t* interpreter, void** local)
{
	mb_value_t first;
	char* haystack = NULL;
	char* needle = NULL;
	int_t start = 1;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_pop_value(interpreter, local, &first));
	if (first.type == MB_DT_INT)
	{
		start = first.value.integer;
		mb_check(mb_pop_string(interpreter, local, &haystack));
		mb_check(mb_pop_string(interpreter, local, &needle));
	}
	else if (first.type == MB_DT_REAL)
	{
		start = static_cast<int_t>(first.value.float_point);
		mb_check(mb_pop_string(interpreter, local, &haystack));
		mb_check(mb_pop_string(interpreter, local, &needle));
	}
	else if (first.type == MB_DT_STRING)
	{
		haystack = first.value.string;
		mb_check(mb_pop_string(interpreter, local, &needle));
	}
	else
	{
		return MB_FUNC_ERR;
	}
	mb_check(mb_attempt_close_bracket(interpreter, local));
	if (!haystack || !needle) return MB_FUNC_ERR;
	if (start < 1) start = 1;
	const size_t offset = static_cast<size_t>(start - 1);
	const std::string text(haystack);
	const std::string::size_type found = offset > text.size()
		? std::string::npos
		: text.find(needle, offset);
	mb_check(mb_push_int(interpreter, local,
		found == std::string::npos ? 0 : static_cast<int_t>(found + 1)));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::data_restore_callback(struct mb_interpreter_t* interpreter, void** local)
{
	int_t offset = 0;
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter || !adapter->ActiveProgram) return MB_FUNC_ERR;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_pop_int(interpreter, local, &offset));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	if (offset < 0 || static_cast<size_t>(offset) > adapter->ActiveProgram->data.size())
	{
		adapter->LastError = "RESTORE offset is out of range";
		return MB_FUNC_ERR;
	}
	adapter->ActiveProgram->data_cursor = static_cast<size_t>(offset);
	mb_check(mb_push_int(interpreter, local, offset));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::data_read_number_callback(struct mb_interpreter_t* interpreter, void** local)
{
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter || !adapter->ActiveProgram) return MB_FUNC_ERR;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	Program* program = adapter->ActiveProgram;
	if (program->data_cursor >= program->data.size())
	{
		adapter->LastError = "READ exhausted DATA";
		return MB_FUNC_ERR;
	}
	const Program::DataValue& value = program->data[program->data_cursor++];
	if (value.is_string)
	{
		adapter->LastError = "READ expected numeric DATA";
		return MB_FUNC_ERR;
	}
	mb_check(mb_push_real(interpreter, local, static_cast<real_t>(value.number)));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::data_read_string_callback(struct mb_interpreter_t* interpreter, void** local)
{
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter || !adapter->ActiveProgram) return MB_FUNC_ERR;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	Program* program = adapter->ActiveProgram;
	if (program->data_cursor >= program->data.size())
	{
		adapter->LastError = "READ exhausted DATA";
		return MB_FUNC_ERR;
	}
	const Program::DataValue& value = program->data[program->data_cursor++];
	if (!value.is_string)
	{
		adapter->LastError = "READ expected string DATA";
		return MB_FUNC_ERR;
	}
	mb_check(mb_push_string(interpreter, local,
		mb_memdup(value.string.c_str(), static_cast<unsigned>(value.string.size() + 1))));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::array_budget_callback(struct mb_interpreter_t* interpreter, void** local)
{
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter || !adapter->ActiveProgram) return MB_FUNC_ERR;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	size_t elements = 1;
	size_t dimensions = 0;
	while (mb_has_arg(interpreter, local))
	{
		mb_value_t value;
		LDBLE number = 0;
		mb_check(mb_pop_value(interpreter, local, &value));
		if (!numeric_value(value, number) || !std::isfinite(number) ||
			number < 1 || std::floor(number) != number ||
			number > static_cast<LDBLE>(std::numeric_limits<size_t>::max()))
		{
			adapter->LastError = "DIM bounds must produce positive integer lengths";
			return MB_FUNC_ERR;
		}
		const size_t length = static_cast<size_t>(number);
		if (length > KERO_ARRAY_ELEMENT_BUDGET || elements > KERO_ARRAY_ELEMENT_BUDGET / length)
		{
			adapter->LastError = "array allocation budget exceeded (1000000 elements)";
			return MB_FUNC_ERR;
		}
		elements *= length;
		++dimensions;
	}
	mb_check(mb_attempt_close_bracket(interpreter, local));
	if (dimensions == 0 || adapter->ArrayElements > KERO_ARRAY_ELEMENT_BUDGET - elements)
	{
		adapter->LastError = "array allocation budget exceeded (1000000 elements)";
		return MB_FUNC_ERR;
	}
	adapter->ArrayElements += elements;
	mb_check(mb_push_int(interpreter, local, 0));
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
	if (description && std::strcmp(description, "No error") == 0) return;
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
	if (adapter->Statements > 1000000)
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

std::string KeroBasicAdapter::transform_source(const char* commands, Program* program)
{
	std::string normalized;
	bool double_quoted = false;
	bool single_quoted = false;
	bool comment = false;
	for (const char* cursor = commands ? commands : ""; *cursor; ++cursor)
	{
		if (*cursor == '\n') comment = false;
		if (!comment && single_quoted && *cursor == '"')
		{
			normalized += "\" + CHR(34) + \"";
			continue;
		}
		if (!comment && !single_quoted && *cursor == '"') double_quoted = !double_quoted;
		if (!comment && !double_quoted && *cursor == '\'')
		{
			single_quoted = !single_quoted;
			normalized += '"';
			continue;
		}
		if (!double_quoted && !single_quoted && *cursor == '#') comment = true;
		bool rem_line = false;
		if (*cursor == ';')
		{
			std::string::size_type start = normalized.find_last_of('\n');
			start = start == std::string::npos ? 0 : start + 1;
			while (start < normalized.size() && std::isspace(static_cast<unsigned char>(normalized[start]))) ++start;
			while (start < normalized.size() && std::isdigit(static_cast<unsigned char>(normalized[start]))) ++start;
			while (start < normalized.size() && std::isspace(static_cast<unsigned char>(normalized[start]))) ++start;
			rem_line = starts_with_word(normalized.substr(start), "REM");
		}
		if ((!double_quoted && !single_quoted || rem_line) && *cursor == ';')
		{
			normalized += '\n';
			double_quoted = false;
			single_quoted = false;
			comment = false;
		}
		else if (*cursor != '\r')
		{
			normalized += *cursor;
		}
	}

	// MY-BASIC has no DATA cursor. Collect PHREEQC BASIC DATA literals in source
	// order and replace RESTORE/READ with adapter callbacks that maintain a
	// cursor per compiled program.
	std::map<std::string, size_t> data_offsets;
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
			if (!is_data) continue;
			data_offsets[line_num] = program->data.size();
			std::string values_str = body.substr(4);
			std::vector<std::string> values;
			std::string current;
			bool in_quote = false;
			for (std::string::size_type i = 0; i < values_str.size(); ++i)
			{
				if (values_str[i] == '"') in_quote = !in_quote;
				if (!in_quote && values_str[i] == ',')
				{
					std::string::size_type s = current.find_first_not_of(" \t");
					std::string::size_type e = current.find_last_not_of(" \t");
					if (s != std::string::npos) values.push_back(current.substr(s, e - s + 1));
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
			for (size_t i = 0; i < values.size(); ++i)
			{
				Program::DataValue value;
				const std::string& literal = values[i];
				if (literal.size() >= 2 && literal[0] == '"' && literal[literal.size() - 1] == '"')
				{
					value.is_string = true;
					value.string = literal.substr(1, literal.size() - 2);
				}
				else
				{
					char* end = NULL;
					value.number = std::strtod(literal.c_str(), &end);
					while (end && *end && std::isspace(static_cast<unsigned char>(*end))) ++end;
					if (!end || end == literal.c_str() || *end)
					{
						if (!evaluate_constant_expression(literal, value.number))
						{
							value.is_string = true;
							value.string = literal;
						}
					}
				}
				program->data.push_back(value);
			}
		}
	}

	// Second pass: transform DATA, RESTORE, and READ into runtime callbacks.
	std::istringstream input(normalized);
	std::ostringstream output;
	std::string line;
	struct ForLoop
	{
		std::string variable;
		std::string step_variable;
		std::string start_label;
		std::string end_label;
	};
	std::vector<ForLoop> for_loops;
	size_t for_loop_number = 0;
	size_t plot_xy_index = 0;
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
			if (starts_with_word(body, "FOR"))
			{
				std::string rest = body.substr(3);
				const std::string::size_type equals = rest.find('=');
				const std::string::size_type to = find_word_ci(rest, "TO", equals == std::string::npos ? 0 : equals + 1);
				const std::string::size_type step = to == std::string::npos
					? std::string::npos
					: find_word_ci(rest, "STEP", to + 2);
				if (equals == std::string::npos || to == std::string::npos)
				{
					LastError = "malformed FOR statement";
					return std::string();
				}
				const std::string::size_type var_start = rest.find_first_not_of(" \t");
				const std::string::size_type var_end = rest.find_last_not_of(" \t", equals - 1);
				const std::string::size_type start_start = rest.find_first_not_of(" \t", equals + 1);
				const std::string::size_type start_end = rest.find_last_not_of(" \t", to - 1);
				const std::string::size_type limit_start = rest.find_first_not_of(" \t", to + 2);
				const std::string::size_type limit_end = rest.find_last_not_of(
					" \t", step == std::string::npos ? rest.size() - 1 : step - 1);
				if (var_start == std::string::npos || var_end == std::string::npos ||
					start_start == std::string::npos || start_end == std::string::npos ||
					limit_start == std::string::npos || limit_end == std::string::npos)
				{
					LastError = "malformed FOR bounds";
					return std::string();
				}
				ForLoop loop;
				loop.variable = translate_identifiers(rest.substr(var_start, var_end - var_start + 1));
				std::ostringstream suffix;
				suffix << for_loop_number++;
				loop.step_variable = "KEROFORSTEP" + suffix.str();
				const std::string limit_variable = "KEROFORLIMIT" + suffix.str();
				loop.start_label = "KEROFORSTART" + suffix.str();
				loop.end_label = "KEROFOREND" + suffix.str();
				const std::string start_expression = translate_identifiers(
					rest.substr(start_start, start_end - start_start + 1));
				const std::string limit_expression = translate_identifiers(
					rest.substr(limit_start, limit_end - limit_start + 1));
				std::string step_expression = "1";
				if (step != std::string::npos)
				{
					const std::string::size_type value_start = rest.find_first_not_of(" \t", step + 4);
					if (value_start == std::string::npos)
					{
						LastError = "FOR STEP requires a value";
						return std::string();
					}
					step_expression = translate_identifiers(rest.substr(value_start));
				}
				output << loop.variable << " = " << start_expression << '\n'
					<< limit_variable << " = " << limit_expression << '\n'
					<< loop.step_variable << " = " << step_expression << '\n'
					<< loop.start_label << ":\n"
					<< "IF ((" << loop.step_variable << " >= 0) AND (" << loop.variable << " > "
					<< limit_variable << ")) OR ((" << loop.step_variable << " < 0) AND ("
					<< loop.variable << " < " << limit_variable << ")) THEN GOTO " << loop.end_label << '\n';
				for_loops.push_back(loop);
				continue;
			}
			if (starts_with_word(body, "NEXT"))
			{
				if (for_loops.empty())
				{
					LastError = "NEXT without FOR";
					return std::string();
				}
				const ForLoop loop = for_loops.back();
				for_loops.pop_back();
				output << loop.variable << " = " << loop.variable << " + " << loop.step_variable << '\n'
					<< "GOTO " << loop.start_label << '\n'
					<< loop.end_label << ":\nREM FOR finished\n";
				continue;
			}
			if (starts_with_word(body, "DATA"))
			{
				output << "REM DATA\n";
				continue;
			}
			if (starts_with_word(body, "RESTORE"))
			{
				// Handled below together with RESTORE following THEN/ELSE.
			}
			if (starts_with_word(body, "READ"))
			{
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

				for (size_t i = 0; i < vars.size(); ++i)
				{
					const bool string_variable = vars[i].find('$') != std::string::npos;
					output << translate_identifiers(vars[i]) << " = "
						<< (string_variable ? "KERODATAREADSTRING()" : "KERODATAREADNUMBER()") << "\n";
				}
				continue;
			}

			// RESTORE is also valid after IF ... THEN, so replace it token-wise.
			std::string replaced;
			bool quoted = false;
			for (std::string::size_type pos = 0; pos < line.size();)
			{
				if (line[pos] == '"')
				{
					quoted = !quoted;
					replaced += line[pos++];
					continue;
				}
				const bool left = pos == 0 || !std::isalnum(static_cast<unsigned char>(line[pos - 1]));
				if (!quoted && left && starts_with_word(line.substr(pos), "RESTORE"))
				{
					pos += 7;
					while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) ++pos;
					std::string target;
					while (pos < line.size() && std::isdigit(static_cast<unsigned char>(line[pos])))
						target += line[pos++];
					size_t offset = 0;
					if (!target.empty())
					{
						std::map<std::string, size_t>::const_iterator found = data_offsets.find(target);
						if (found == data_offsets.end())
						{
							LastError = std::string("RESTORE target has no DATA line: ") + target;
							return std::string();
						}
						offset = found->second;
					}
					std::ostringstream call;
					call << "KERODATARESTORE(" << offset << ")";
					replaced += call.str();
					continue;
				}
				replaced += line[pos++];
			}
			line = replaced;
		}
		output << transform_statement(line, &plot_xy_index) << '\n';
	}
	if (!for_loops.empty())
	{
		LastError = "FOR without NEXT";
		return std::string();
	}
	output << "KEROPROGRAMEND:\nREM program finished\n";
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
				// PHREEQC BASIC represents an unavailable phase index with its
				// conventional sentinel so scripts can continue conditionally.
				value = -999.999;
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
	case VALUE_PHASE_DELTA_H:
		value = adapter->PhreeqcPtr->calc_deltah_p(name);
		break;
	case VALUE_DH_A0:
		value = adapter->PhreeqcPtr->dh_a0(name);
		break;
	case VALUE_DH_BDOT:
		value = adapter->PhreeqcPtr->dh_bdot(name);
		break;
	case VALUE_DIFF_C:
		value = adapter->PhreeqcPtr->diff_c(name);
		break;
	case VALUE_EQUI_DELTA:
		value = adapter->PhreeqcPtr->equi_phase_delta(name);
		break;
	case VALUE_GAMMA:
		value = adapter->PhreeqcPtr->activity_coefficient(name);
		break;
	case VALUE_KIN_DELTA:
		value = adapter->PhreeqcPtr->kinetics_moles_delta(name);
		break;
	case VALUE_LOG_GAMMA:
		value = adapter->PhreeqcPtr->log_activity_coefficient(name);
		break;
	case VALUE_LK_NAMED:
		value = adapter->PhreeqcPtr->calc_logk_n(name);
		break;
	case VALUE_LK_PHASE:
		value = adapter->PhreeqcPtr->calc_logk_p(name);
		break;
	case VALUE_LK_SPECIES:
		value = adapter->PhreeqcPtr->calc_logk_s(name);
		break;
	case VALUE_PHASE_VM:
		value = adapter->PhreeqcPtr->phase_vm(name);
		break;
	case VALUE_PR_PHI:
		value = adapter->PhreeqcPtr->pr_phi(name);
		break;
	case VALUE_T_SC:
		value = adapter->PhreeqcPtr->calc_t_sc(name);
		break;
	}
	mb_check(mb_push_real(interpreter, local, static_cast<real_t>(value)));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::scalar_value_callback(
	struct mb_interpreter_t* interpreter,
	void** local,
	ScalarValue kind)
{
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	LDBLE value = 0;
	switch (kind)
	{
	case SCALAR_ALK:
		value = adapter->PhreeqcPtr->mass_water_aq_x != 0
			? adapter->PhreeqcPtr->total_alkalinity / adapter->PhreeqcPtr->mass_water_aq_x : 0;
		break;
	case SCALAR_APHI: value = adapter->PhreeqcPtr->A0; break;
	case SCALAR_CHARGE_BALANCE: value = adapter->PhreeqcPtr->cb_x; break;
	case SCALAR_CURRENT_A: value = adapter->PhreeqcPtr->current_A; break;
	case SCALAR_DH_A: value = adapter->PhreeqcPtr->llnl_temp.empty() ? adapter->PhreeqcPtr->DH_A : adapter->PhreeqcPtr->a_llnl; break;
	case SCALAR_DH_AV: value = adapter->PhreeqcPtr->DH_Av; break;
	case SCALAR_DH_B: value = adapter->PhreeqcPtr->llnl_temp.empty() ? adapter->PhreeqcPtr->DH_B : adapter->PhreeqcPtr->b_llnl; break;
	case SCALAR_EPS_R: value = adapter->PhreeqcPtr->eps_r; break;
	case SCALAR_ITERATIONS: value = adapter->PhreeqcPtr->overall_iterations; break;
	case SCALAR_KAPPA: value = adapter->PhreeqcPtr->kappa_0; break;
	case SCALAR_KIN_TIME: value = adapter->PhreeqcPtr->rate_kin_time; break;
	case SCALAR_MU: value = adapter->PhreeqcPtr->mu_x; break;
	case SCALAR_OSMOTIC:
		value = adapter->PhreeqcPtr->pitzer_model == TRUE || adapter->PhreeqcPtr->sit_model == TRUE
			? adapter->PhreeqcPtr->COSMOT : 0;
		break;
	case SCALAR_PERCENT_ERROR:
		value = adapter->PhreeqcPtr->total_ions_x != 0
			? 100 * adapter->PhreeqcPtr->cb_x / adapter->PhreeqcPtr->total_ions_x : 0;
		break;
	case SCALAR_POT_V:
		value = adapter->PhreeqcPtr->use.Get_solution_ptr()
			? adapter->PhreeqcPtr->use.Get_solution_ptr()->Get_potV() : 0;
		break;
	case SCALAR_PRESSURE: value = adapter->PhreeqcPtr->pressure(); break;
	case SCALAR_QBRN: value = adapter->PhreeqcPtr->QBrn; break;
	case SCALAR_RHO: value = adapter->PhreeqcPtr->calc_dens(); break;
	case SCALAR_RHO_0: value = adapter->PhreeqcPtr->rho_0; break;
	case SCALAR_SC: value = adapter->PhreeqcPtr->calc_SC(); break;
	}
	mb_check(mb_push_real(interpreter, local, static_cast<real_t>(value)));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::f_visc_callback(struct mb_interpreter_t* interpreter, void** local)
{
	char* name = NULL;
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_pop_string(interpreter, local, &name));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	if (!name) return MB_FUNC_ERR;
	mb_check(mb_push_real(interpreter, local,
		static_cast<real_t>(adapter->PhreeqcPtr->calc_f_visc(name))));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::mcd_jtot_callback(struct mb_interpreter_t* interpreter, void** local)
{
	char* name = NULL;
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_pop_string(interpreter, local, &name));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	const LDBLE value = name && adapter->PhreeqcPtr->state == TRANSPORT && adapter->PhreeqcPtr->multi_Dflag
		? adapter->PhreeqcPtr->flux_mcd(name, 1) : 0;
	mb_check(mb_push_real(interpreter, local, static_cast<real_t>(value)));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::mcd_jconc_callback(struct mb_interpreter_t* interpreter, void** local)
{
	char* name = NULL;
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_pop_string(interpreter, local, &name));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	const LDBLE value = name && adapter->PhreeqcPtr->state == TRANSPORT && adapter->PhreeqcPtr->multi_Dflag
		? adapter->PhreeqcPtr->flux_mcd(name, 2) : 0;
	mb_check(mb_push_real(interpreter, local, static_cast<real_t>(value)));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::setdiff_c_callback(struct mb_interpreter_t* interpreter, void** local)
{
	char* name = NULL;
	mb_value_t d_value;
	mb_value_t dv_value;
	LDBLE d = 0;
	LDBLE dv = 0;
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_pop_string(interpreter, local, &name));
	mb_check(mb_pop_value(interpreter, local, &d_value));
	if (!numeric_value(d_value, d)) return MB_FUNC_ERR;
	if (mb_has_arg(interpreter, local))
	{
		mb_check(mb_pop_value(interpreter, local, &dv_value));
		if (!numeric_value(dv_value, dv)) return MB_FUNC_ERR;
	}
	mb_check(mb_attempt_close_bracket(interpreter, local));
	if (!name) return MB_FUNC_ERR;
	mb_check(mb_push_real(interpreter, local,
		static_cast<real_t>(adapter->PhreeqcPtr->setdiff_c(name, d, dv))));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::get_por_callback(struct mb_interpreter_t* interpreter, void** local)
{
	int_t cell = 0;
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_pop_int(interpreter, local, &cell));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	LDBLE value = 0;
	if (adapter->PhreeqcPtr->phast == TRUE) value = adapter->PhreeqcPtr->cell_porosity;
	else if (cell > 0 && static_cast<size_t>(cell) < adapter->PhreeqcPtr->cell_data.size() &&
		cell != adapter->PhreeqcPtr->count_cells + 1)
		value = adapter->PhreeqcPtr->cell_data[static_cast<size_t>(cell)].por;
	mb_check(mb_push_real(interpreter, local, static_cast<real_t>(value)));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::change_por_callback(struct mb_interpreter_t* interpreter, void** local)
{
	mb_value_t porosity_value;
	LDBLE porosity = 0;
	int_t cell = 0;
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_pop_value(interpreter, local, &porosity_value));
	mb_check(mb_pop_int(interpreter, local, &cell));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	if (!numeric_value(porosity_value, porosity)) return MB_FUNC_ERR;
	if (cell > 0 && static_cast<size_t>(cell) < adapter->PhreeqcPtr->cell_data.size() &&
		cell != adapter->PhreeqcPtr->count_cells + 1)
		adapter->PhreeqcPtr->cell_data[static_cast<size_t>(cell)].por = porosity;
	mb_check(mb_push_real(interpreter, local, static_cast<real_t>(porosity)));
	return MB_FUNC_OK;
}

int KeroBasicAdapter::add_heading_callback(struct mb_interpreter_t* interpreter, void** local)
{
	char* heading = NULL;
	KeroBasicAdapter* adapter = from_interpreter(interpreter);
	if (!adapter) return MB_FUNC_ERR;
	mb_check(mb_attempt_open_bracket(interpreter, local));
	mb_check(mb_pop_string(interpreter, local, &heading));
	mb_check(mb_attempt_close_bracket(interpreter, local));
	int count = 0;
	if (heading && adapter->PhreeqcPtr->current_user_punch)
	{
		adapter->PhreeqcPtr->current_user_punch->Get_headings().push_back(heading);
		count = static_cast<int>(adapter->PhreeqcPtr->current_user_punch->Get_headings().size());
	}
	mb_check(mb_push_int(interpreter, local, count));
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
