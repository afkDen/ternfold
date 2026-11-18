#ifndef TERNFOLD_EXEC_DRIVER_RUN_H
#define TERNFOLD_EXEC_DRIVER_RUN_H

#include "ternfold/diagnostics.h"
#include "ternfold/interpreter.h"

/* Uses an already verified program and the successful frontend diagnostics.
 * Returns the CLI status; when *failure is set, the caller reports host exit 2. */
int tf_run_verified(const TfVerifiedCore *program, const TfDiagnosticBag *frontend,
                    const char *input_path, TfDiagnosticFormat format, const char **failure);
int tf_driver_missing_entry(const TfDiagnosticBag *frontend, TfDiagnosticFormat format,
                            const char *message, const char **failure);

#endif
