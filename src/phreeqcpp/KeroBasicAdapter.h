#ifndef KEROTAKIS_MY_BASIC_ADAPTER_H
#define KEROTAKIS_MY_BASIC_ADAPTER_H

#include "BasicInterpreter.h"
#include "my_basic.h"

#include <chrono>
#include <set>
#include <string>
#include <vector>

class Phreeqc;

// Clean-room PHREEQC-facing adapter for the pinned MIT MY-BASIC core. During
// migration, unsupported dialect constructs fail explicitly instead of
// silently falling back to the legacy engine.
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
		VALUE_SPECIES_DELTA_H,
		VALUE_PHASE_DELTA_H,
		VALUE_DH_A0,
		VALUE_DH_BDOT,
		VALUE_DIFF_C,
		VALUE_EQUI_DELTA,
		VALUE_GAMMA,
		VALUE_KIN_DELTA,
		VALUE_LOG_GAMMA,
		VALUE_LK_NAMED,
		VALUE_LK_PHASE,
		VALUE_LK_SPECIES,
		VALUE_PHASE_VM,
		VALUE_PR_PHI,
		VALUE_T_SC
	};
	enum ScalarValue
	{
		SCALAR_ALK, SCALAR_APHI, SCALAR_CHARGE_BALANCE, SCALAR_CURRENT_A,
		SCALAR_DH_A, SCALAR_DH_AV, SCALAR_DH_B, SCALAR_EPS_R,
		SCALAR_ITERATIONS, SCALAR_KAPPA, SCALAR_KIN_TIME, SCALAR_MU,
		SCALAR_OSMOTIC, SCALAR_PERCENT_ERROR, SCALAR_POT_V, SCALAR_PRESSURE,
		SCALAR_QBRN, SCALAR_RHO, SCALAR_RHO_0, SCALAR_SC
	};

	static int save_callback(struct mb_interpreter_t* interpreter, void** local);
	static int punch_callback(struct mb_interpreter_t* interpreter, void** local);
	static int line_print_callback(struct mb_interpreter_t* interpreter, void** local);
	static int graph_x_callback(struct mb_interpreter_t* interpreter, void** local);
	static int graph_y_callback(struct mb_interpreter_t* interpreter, void** local);
	static int graph_sy_callback(struct mb_interpreter_t* interpreter, void** local);
	static int graph_y_common(struct mb_interpreter_t* interpreter, void** local, bool secondary);
	static int plot_xy_callback(struct mb_interpreter_t* interpreter, void** local);
	static int parm_callback(struct mb_interpreter_t* interpreter, void** local);
	static int get_callback(struct mb_interpreter_t* interpreter, void** local);
	static int put_callback(struct mb_interpreter_t* interpreter, void** local);
	static int get_string_callback(struct mb_interpreter_t* interpreter, void** local);
	static int put_string_callback(struct mb_interpreter_t* interpreter, void** local);
	static int exists_callback(struct mb_interpreter_t* interpreter, void** local);
	static int eol_callback(struct mb_interpreter_t* interpreter, void** local);
	static int eol_notab_callback(struct mb_interpreter_t* interpreter, void** local);
	static int no_newline_callback(struct mb_interpreter_t* interpreter, void** local);
	static int str_e_callback(struct mb_interpreter_t* interpreter, void** local);
	static int str_f_callback(struct mb_interpreter_t* interpreter, void** local);
	static int pad_callback(struct mb_interpreter_t* interpreter, void** local);
	static int trim_callback(struct mb_interpreter_t* interpreter, void** local);
	static int ltrim_callback(struct mb_interpreter_t* interpreter, void** local);
	static int rtrim_callback(struct mb_interpreter_t* interpreter, void** local);
	static int iso_unit_callback(struct mb_interpreter_t* interpreter, void** local);
	static int title_callback(struct mb_interpreter_t* interpreter, void** local);
	static int description_callback(struct mb_interpreter_t* interpreter, void** local);
	static int list_ss_callback(struct mb_interpreter_t* interpreter, void** local);
	static int edl_species_callback(struct mb_interpreter_t* interpreter, void** local);
	static int system_total_callback(struct mb_interpreter_t* interpreter, void** local);
	static int equivalent_fraction_callback(struct mb_interpreter_t* interpreter, void** local);
	static int mean_gamma_callback(struct mb_interpreter_t* interpreter, void** local);
	static int debye_length_callback(struct mb_interpreter_t* interpreter, void** local);
	static int activity_callback(struct mb_interpreter_t* interpreter, void** local);
	static int molality_callback(struct mb_interpreter_t* interpreter, void** local);
	static int total_callback(struct mb_interpreter_t* interpreter, void** local);
	static int saturation_index_callback(struct mb_interpreter_t* interpreter, void** local);
	static int saturation_ratio_callback(struct mb_interpreter_t* interpreter, void** local);
	static int log_molality_callback(struct mb_interpreter_t* interpreter, void** local);
	static int species_delta_h_callback(struct mb_interpreter_t* interpreter, void** local);
	static int phase_delta_h_callback(struct mb_interpreter_t* interpreter, void** local);
	static int dh_a0_callback(struct mb_interpreter_t* interpreter, void** local);
	static int dh_bdot_callback(struct mb_interpreter_t* interpreter, void** local);
	static int diff_c_callback(struct mb_interpreter_t* interpreter, void** local);
	static int equi_delta_callback(struct mb_interpreter_t* interpreter, void** local);
	static int gamma_callback(struct mb_interpreter_t* interpreter, void** local);
	static int kin_delta_callback(struct mb_interpreter_t* interpreter, void** local);
	static int log_gamma_callback(struct mb_interpreter_t* interpreter, void** local);
	static int lk_named_callback(struct mb_interpreter_t* interpreter, void** local);
	static int lk_phase_callback(struct mb_interpreter_t* interpreter, void** local);
	static int lk_species_callback(struct mb_interpreter_t* interpreter, void** local);
	static int phase_vm_callback(struct mb_interpreter_t* interpreter, void** local);
	static int pr_phi_callback(struct mb_interpreter_t* interpreter, void** local);
	static int t_sc_callback(struct mb_interpreter_t* interpreter, void** local);
	static int log10_callback(struct mb_interpreter_t* interpreter, void** local);
	static int log_activity_callback(struct mb_interpreter_t* interpreter, void** local);
	static int kinetics_moles_callback(struct mb_interpreter_t* interpreter, void** local);
	static int equi_phase_callback(struct mb_interpreter_t* interpreter, void** local);
	static int gas_callback(struct mb_interpreter_t* interpreter, void** local);
	static int ss_callback(struct mb_interpreter_t* interpreter, void** local);
	static int gfw_callback(struct mb_interpreter_t* interpreter, void** local);
	static int phase_formula_callback(struct mb_interpreter_t* interpreter, void** local);
	static int kinetics_formula_callback(struct mb_interpreter_t* interpreter, void** local);
	static int species_formula_callback(struct mb_interpreter_t* interpreter, void** local);
	static int phase_equation_callback(struct mb_interpreter_t* interpreter, void** local);
	static int species_equation_callback(struct mb_interpreter_t* interpreter, void** local);
	static int calc_value_callback(struct mb_interpreter_t* interpreter, void** local);
	static int sum_species_callback(struct mb_interpreter_t* interpreter, void** local);
	static int sum_gas_callback(struct mb_interpreter_t* interpreter, void** local);
	static int sum_ss_callback(struct mb_interpreter_t* interpreter, void** local);
	static int surface_callback(struct mb_interpreter_t* interpreter, void** local);
	static int edl_callback(struct mb_interpreter_t* interpreter, void** local);
	static int misc1_callback(struct mb_interpreter_t* interpreter, void** local);
	static int misc2_callback(struct mb_interpreter_t* interpreter, void** local);
	static int isotope_callback(struct mb_interpreter_t* interpreter, void** local);
	static int partial_pressure_callback(struct mb_interpreter_t* interpreter, void** local);
	static int gas_pressure_callback(struct mb_interpreter_t* interpreter, void** local);
	static int gas_molar_volume_callback(struct mb_interpreter_t* interpreter, void** local);
	static int cell_no_callback(struct mb_interpreter_t* interpreter, void** local);
	static int step_no_callback(struct mb_interpreter_t* interpreter, void** local);
	static int sim_no_callback(struct mb_interpreter_t* interpreter, void** local);
	static int distance_callback(struct mb_interpreter_t* interpreter, void** local);
	static int reaction_increment_callback(struct mb_interpreter_t* interpreter, void** local);
	static int alk_callback(struct mb_interpreter_t* interpreter, void** local);
	static int aphi_callback(struct mb_interpreter_t* interpreter, void** local);
	static int charge_balance_callback(struct mb_interpreter_t* interpreter, void** local);
	static int current_a_callback(struct mb_interpreter_t* interpreter, void** local);
	static int dh_a_callback(struct mb_interpreter_t* interpreter, void** local);
	static int dh_av_callback(struct mb_interpreter_t* interpreter, void** local);
	static int dh_b_callback(struct mb_interpreter_t* interpreter, void** local);
	static int eps_r_callback(struct mb_interpreter_t* interpreter, void** local);
	static int iterations_callback(struct mb_interpreter_t* interpreter, void** local);
	static int kappa_callback(struct mb_interpreter_t* interpreter, void** local);
	static int kin_time_callback(struct mb_interpreter_t* interpreter, void** local);
	static int mu_callback(struct mb_interpreter_t* interpreter, void** local);
	static int osmotic_callback(struct mb_interpreter_t* interpreter, void** local);
	static int percent_error_callback(struct mb_interpreter_t* interpreter, void** local);
	static int pot_v_callback(struct mb_interpreter_t* interpreter, void** local);
	static int pressure_callback(struct mb_interpreter_t* interpreter, void** local);
	static int qbrn_callback(struct mb_interpreter_t* interpreter, void** local);
	static int rho_callback(struct mb_interpreter_t* interpreter, void** local);
	static int rho_0_callback(struct mb_interpreter_t* interpreter, void** local);
	static int sc_callback(struct mb_interpreter_t* interpreter, void** local);
	static int f_visc_callback(struct mb_interpreter_t* interpreter, void** local);
	static int mcd_jtot_callback(struct mb_interpreter_t* interpreter, void** local);
	static int mcd_jconc_callback(struct mb_interpreter_t* interpreter, void** local);
	static int setdiff_c_callback(struct mb_interpreter_t* interpreter, void** local);
	static int get_por_callback(struct mb_interpreter_t* interpreter, void** local);
	static int change_por_callback(struct mb_interpreter_t* interpreter, void** local);
	static int add_heading_callback(struct mb_interpreter_t* interpreter, void** local);
	static int soln_vol_callback(struct mb_interpreter_t* interpreter, void** local);
	static int sim_time_callback(struct mb_interpreter_t* interpreter, void** local);
	static int total_time_callback(struct mb_interpreter_t* interpreter, void** local);
	static int mid_string_callback(struct mb_interpreter_t* interpreter, void** local);
	static int instr_callback(struct mb_interpreter_t* interpreter, void** local);
	static int data_restore_callback(struct mb_interpreter_t* interpreter, void** local);
	static int data_read_number_callback(struct mb_interpreter_t* interpreter, void** local);
	static int data_read_string_callback(struct mb_interpreter_t* interpreter, void** local);
	static int array_budget_callback(struct mb_interpreter_t* interpreter, void** local);
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
	static int step_callback(
		struct mb_interpreter_t* interpreter,
		void** local,
		const char* file,
		int position,
		unsigned short row,
		unsigned short column);

	std::string transform_source(const char* commands, Program* program);
	static bool is_dispose_command(const char* commands);
	static KeroBasicAdapter* from_interpreter(struct mb_interpreter_t* interpreter);
	static int named_chemistry_callback(
		struct mb_interpreter_t* interpreter,
		void** local,
		ChemistryValue kind);
	static int scalar_value_callback(
		struct mb_interpreter_t* interpreter,
		void** local,
		ScalarValue kind);
	static int set_count_and_arrays(
		struct mb_interpreter_t* interpreter,
		void** local,
		void* count_var,
		void* names_var,
		void* values_var,
		const std::vector<std::string>& names,
		const std::vector<double>& values);

	int report_error(const std::string& context);
	int set_runtime_values(Program* program);
	double sim_time_value() const;
	double total_time_value() const;
	void destroy_program(Program* program);

	Phreeqc* PhreeqcPtr;
	std::set<Program*> Programs;
	Program* ActiveProgram;
	std::string LastError;
	int RecursionDepth;
	size_t OutputBytes;
	size_t Statements;
	size_t ArrayElements;
	std::chrono::steady_clock::time_point Deadline;
};

#endif
