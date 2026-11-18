#ifndef TERNFOLD_REPL_H
#define TERNFOLD_REPL_H

#include <stdbool.h>
#include <stdio.h>

/* The driver supplies binary streams (including stdin on Windows) and an
 * optional local import root. Commands never invoke source main implicitly. */
int tf_repl(FILE *in,FILE *out,FILE *err,const char *root,bool prompt);

#endif
