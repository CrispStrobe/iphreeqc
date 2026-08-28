// Modified by Christian Ströbele and Kerotakis contributors, 2026-08-21 to 2026-08-22: checked thermochemistry, chart export, and bounded MY-BASIC integration; see git history for the exact extent.
#ifndef KEROTAKIS_DISABLED_BASIC_H
#define KEROTAKIS_DISABLED_BASIC_H

#include "BasicInterpreter.h"

class Phreeqc;

// Rejecting backend for equilibrium-only builds. It never parses or executes
// program text and deliberately allocates no compiled-program state.
class DisabledBasic : public BasicInterpreter
{
public:
	explicit DisabledBasic(Phreeqc* phreeqc);
	virtual ~DisabledBasic() {}

	virtual int basic_compile(const char* commands, void** lnbase, void** vbase, void** lpbase);
	virtual int basic_run(char* commands, void* lnbase, void* vbase, void* lpbase);

private:
	int reject();
	Phreeqc* PhreeqcPtr;
};

#endif
