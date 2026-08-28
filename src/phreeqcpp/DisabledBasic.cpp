// Modified by Christian Ströbele and Kerotakis contributors, 2026-08-21 to 2026-08-22: checked thermochemistry, chart export, and bounded MY-BASIC integration; see git history for the exact extent.
#include "DisabledBasic.h"
#include "Phreeqc.h"

DisabledBasic::DisabledBasic(Phreeqc* phreeqc)
	: PhreeqcPtr(phreeqc)
{
}

int DisabledBasic::basic_compile(const char*, void** lnbase, void** vbase, void** lpbase)
{
	if (lnbase) *lnbase = NULL;
	if (vbase) *vbase = NULL;
	if (lpbase) *lpbase = NULL;
	return this->reject();
}

int DisabledBasic::basic_run(char*, void*, void*, void*)
{
	return this->reject();
}

int DisabledBasic::reject()
{
	this->PhreeqcPtr->error_msg(
		"PHREEQC BASIC capability is disabled; RATES/KINETICS, USER_PUNCH, "
		"USER_PRINT, CALCULATE_VALUES, and USER_GRAPH programs cannot execute.",
		CONTINUE);
	return 1;
}
