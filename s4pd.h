#include <stdlib.h>

#include "m_pd.h"
#include "s7.h"
#include "string.h"
#include "time.h"

#define MAX_OUTLETS 32
#define MAX_ATOMS_PER_MESSAGE 1024
#define MAX_ATOMS_PER_OUTPUT_LIST 1024

// for silencing unused param warnings
#define UNUSED(x) (void)(x)

static t_class *s4pd_class;

// struct used (as void pointer) for clock scheduling
typedef struct _s4pd_clock_info {
    // t_s4pd *obj;
    void *obj;
    t_symbol *handle;
    t_clock *clock;
    struct _s4pd_clock_info *previous;
    struct _s4pd_clock_info *next;
} t_s4pd_clock_info;

typedef struct _s4pd {
    t_object x_obj;
    s7_scheme *s7;  // pointer to the s7 instance
    bool log_repl;  // whether to automatically post return values from S7
                    // interpreter to console
    bool log_null;  // whether to log return values that are null, unspecified,
                    // or gensyms
    int num_outlets;
    t_outlet *outlets[MAX_OUTLETS];
    t_symbol *filename;

    t_canvas *x_canvas;
    t_symbol *extern_dir;  // FUTURE: directory of the external

    t_s4pd_clock_info *first_clock;  // DUL of clocks
    t_s4pd_clock_info *last_clock;   // keep pointer to most recent clock
} t_s4pd;

// conversion functions
int s7_obj_to_atom(s7_scheme *s7, s7_pointer *s7_obj, t_atom *atom);
s7_pointer atom_to_s7_obj(s7_scheme *s7, t_atom *ap);

// main external methods
void *s4pd_new(t_symbol *s, int argc, t_atom *argv);
void s4pd_free(t_s4pd *x);
void s4pd_init_s7(t_s4pd *x);
void s4pd_load_from_path(t_s4pd *x, const char *filename);
void s4pd_s7_load(t_s4pd *x, char *full_path);
void s4pd_post_s7_res(t_s4pd *x, s7_pointer res);
void s4pd_s7_eval_string(t_s4pd *x, char *string_to_eval);
void s4pd_s7_call(t_s4pd *x, s7_pointer funct, s7_pointer args);
void s4pd_eval_atoms_as_string(t_s4pd *x, t_symbol *s, long argc, t_atom *argv);

// pd message methods
void s4pd_reset(t_s4pd *x);
void s4pd_log_null(t_s4pd *x, t_floatarg f);
void s4pd_log_repl(t_s4pd *x, t_floatarg f);
void s4pd_read(t_s4pd *x, t_symbol *s);
void s4pd_message(t_s4pd *x, t_symbol *s, int argc, t_atom *argv);

// s7 FFI functions
static s7_pointer s7_load_from_path(s7_scheme *s7, s7_pointer args);
static s7_pointer s7_pd_output(s7_scheme *s7, s7_pointer args);
static s7_pointer s7_post(s7_scheme *s7, s7_pointer args);
static s7_pointer s7_send(s7_scheme *s7, s7_pointer args);
static s7_pointer s7_table_read(s7_scheme *s7, s7_pointer args);
static s7_pointer s7_table_write(s7_scheme *s7, s7_pointer args);
static s7_pointer s7_schedule_delay(s7_scheme *s7, s7_pointer args);
static s7_pointer s7_cancel_delay(s7_scheme *s7, s7_pointer args);
static s7_pointer s7_cancel_clocks(s7_scheme *s7, s7_pointer args);

// schedule/clock
void s4pd_clock_callback(void *arg);
void s4pd_remove_clock(t_s4pd *x, t_s4pd_clock_info *clock_info);
void s4pd_cancel_clocks(t_s4pd *x);

/********************************************************************************/
// some helpers for string/symbol handling

// return true if a string begins and ends with quotes
int in_quotes(const char *string);
char *trim_quotes(const char *input);

// return true if a string starts with a single quote
int is_quoted_symbol(const char *string);

// return string of input dropping symbol quote
char *trim_symbol_quote(const char *input);

/********************************************************************************/
// main Pd boilerplate
void s4pd_setup(void);

void *s4pd_new(t_symbol *s, int argc, t_atom *argv);

void s4pd_free(t_s4pd *x);

void s4pd_init_s7(t_s4pd *x);

void s4pd_read(t_s4pd *x, t_symbol *s);

// get a pd struct pointer from the s7 environment pointer
t_s4pd *get_pd_obj(s7_scheme *s7);

// convert a Pd atom to the appropriate type of s7 pointer
// only handles basic types for now but is working
s7_pointer atom_to_s7_obj(s7_scheme *s7, t_atom *ap);

int s7_obj_to_atom(s7_scheme *s7, s7_pointer *s7_obj, t_atom *atom);

void s4pd_reset(t_s4pd *x);

// the generic message handler that evaluates input as code
void s4pd_message(t_s4pd *x, t_symbol *s, int argc, t_atom *argv);

// 2021-11-22 - something wrong here, when receiving messages this way
// and using delay, it's crashing.
void s4pd_eval_atoms_as_string(t_s4pd *x, t_symbol *s, long argc, t_atom *argv);

static s7_pointer s7_load_from_path(s7_scheme *s7, s7_pointer args);

// send generic output out an outlet
static s7_pointer s7_post(s7_scheme *s7, s7_pointer args);

// send a message to a receiver
static s7_pointer s7_send(s7_scheme *s7, s7_pointer args);

// read a value from an Pd array
static s7_pointer s7_table_read(s7_scheme *s7, s7_pointer args);

// write a float to a Pd array
static s7_pointer s7_table_write(s7_scheme *s7, s7_pointer args);

// send output out an outlet
static s7_pointer s7_pd_output(s7_scheme *s7, s7_pointer args);

void s4pd_log_null(t_s4pd *x, t_floatarg arg);

void s4pd_log_repl(t_s4pd *x, t_floatarg arg);

void s4pd_post_s7_res(t_s4pd *x, s7_pointer res);

// eval string  with error logging
void s4pd_s7_eval_string(t_s4pd *x, char *string_to_eval);

// call s7_call, with error logging
void s4pd_s7_call(t_s4pd *x, s7_pointer funct, s7_pointer args);

// call s7_load using the pd searchpath
void s4pd_load_from_path(t_s4pd *x, const char *filename);

/*********************************************************************************
 * Scheduler and clock stuff */

// delay a function using Pd clock objects for floating point precision delays
// called from scheme as (s4pd-schedule-delay)
static s7_pointer s7_schedule_delay(s7_scheme *s7, s7_pointer args);

// the callback that runs for any clock and is used to find the delayed function
// in Scheme
void s4pd_clock_callback(void *arg);

// remove a clock_info pointer from the queue, updating queue head and tail
// this just extracts the clock, which could be anywhere in the queue
void s4pd_remove_clock(t_s4pd *x, t_s4pd_clock_info *clock_info);

void s4pd_cancel_clocks(t_s4pd *x);

// s7 method for cancelling clocks
static s7_pointer s7_cancel_clocks(s7_scheme *s7, s7_pointer args);
