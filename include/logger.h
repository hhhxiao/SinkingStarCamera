#ifndef ORTHOCAM_LOGGER_H
#define ORTHOCAM_LOGGER_H

// printf-style logging. Writes to <dll dir>\orthocam.log and to the debugger,
// plus stdout when a console is attached.
#define LOG(...) log_msg(LOG_LEVEL_INFO, __FUNCTION__, __VA_ARGS__)
#define ERR(...) log_msg(LOG_LEVEL_ERROR, __FUNCTION__, __VA_ARGS__)
#define DBG(...) log_msg(LOG_LEVEL_DEBUG, __FUNCTION__, __VA_ARGS__)
#define TRACE(...) log_msg(LOG_LEVEL_TRACE, __FUNCTION__, __VA_ARGS__)

enum LogLevel { LOG_LEVEL_TRACE = 0, LOG_LEVEL_DEBUG, LOG_LEVEL_INFO, LOG_LEVEL_ERROR };

// Opens the log file. Safe to call more than once.
void init_logger();
// Sets the minimum level that actually gets written.
void set_log_level(LogLevel level);
void log_msg(int level, const char *function_name, const char *fmt, ...);

#endif
