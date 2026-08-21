#ifndef KEROTAKIS_BASIC_INTERPRETER_H
#define KEROTAKIS_BASIC_INTERPRETER_H

// Interpreter boundary owned by Kerotakis. Implementations may compile and
// execute PHREEQC BASIC programs, or reject them explicitly in a no-BASIC
// build. The opaque program pointers preserve PHREEQC's existing call sites.
class BasicInterpreter
{
public:
	virtual ~BasicInterpreter() {}
	virtual int basic_compile(const char* commands, void** lnbase, void** vbase, void** lpbase) = 0;
	virtual int basic_run(char* commands, void* lnbase, void* vbase, void* lpbase) = 0;
};

#endif
