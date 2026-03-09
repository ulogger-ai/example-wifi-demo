#ifndef _LOGGING_H_
#define _LOGGING_H_

void logging_init_local(void);
void generate_init_logs_local(void);
void logging_generate_test_logs(void);
void log_local(const char *format, ...);

#endif
