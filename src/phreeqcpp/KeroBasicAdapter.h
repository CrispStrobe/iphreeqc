#ifndef KEROTAKIS_MY_BASIC_ADAPTER_H
#define KEROTAKIS_MY_BASIC_ADAPTER_H

#include "BasicInterpreter.h"
#include "my_basic.h"

#include <set>
#include <string>

class Phreeqc;

// Clean-room PHREEQC-facing adapter for the pinned MIT MY-BASIC core. This is
// a deliberately small compatibility preview; unsupported dialect constructs
// fail during MY-BASIC parsing instead of falling back to the legacy engine.
class KeroBasicAdapter : public BasicInterpreter
{
public:
	explicit KeroBasicAdapter(Phreeqc* phreeqc);
	virtual ~KeroBasicAdapter();

	virtual int basic_compile(const char* commands, void** lnbase, void** vbase, void** lpbase);
	virtual int basic_run(char* commands, void* lnbase, void* vbase, void* lpbase);

private:
	struct Program;
	enum ChemistryValue
	{
		VALUE_ACTIVITY,
		VALUE_MOLALITY,
		VALUE_TOTAL,
		VALUE_SATURATION_INDEX,
		VALUE_SATURATION_RATIO,
		VALUE_LOG_MOLALITY,
		VALUE_SPECIES_DELTA_H
	};

	static int save_callback(struct mb_interpreter_t* interpreter, void** local);
	static int punch_callback(struct mb_interpreter_t* interpreter, void** local);
	static int parm_callback(struct mb_interpreter_t* interpreter, void** local);
	static int activity_callback(struct mb_interpreter_t* interpreter, void** local);
	static int molality_callback(struct mb_interpreter_t* interpreter, void** local);
	static int total_callback(struct mb_interpreter_t* interpreter, void** local);
	static int saturation_index_callback(struct mb_interpreter_t* interpreter, void** local);
	static int saturation_ratio_callback(struct mb_interpreter_t* interpreter, void** local);
	static int log_molality_callback(struct mb_interpreter_t* interpreter, void** local);
	static int species_delta_h_callback(struct mb_interpreter_t* interpreter, void** local);
	static int print_callback(struct mb_interpreter_t* interpreter, const char* format, ...);
	static int input_callback(struct mb_interpreter_t* interpreter, const char* prompt, char* buffer, int length);
	static int import_callback(struct mb_interpreter_t* interpreter, const char* path);
	static void error_callback(
		struct mb_interpreter_t* interpreter,
		mb_error_e error,
		const char* description,
		const char* file,
		int position,
		unsigned short row,
		unsigned short column,
		int abort_code);

	static std::string transform_source(const char* commands);
	static bool is_dispose_command(const char* commands);
	static KeroBasicAdapter* from_interpreter(struct mb_interpreter_t* interpreter);
	static int named_chemistry_callback(
		struct mb_interpreter_t* interpreter,
		void** local,
		ChemistryValue kind);

	int report_error(const std::string& context);
	int set_runtime_values(Program* program);
	void destroy_program(Program* program);

	Phreeqc* PhreeqcPtr;
	std::set<Program*> Programs;
	std::string LastError;
};

#endif
