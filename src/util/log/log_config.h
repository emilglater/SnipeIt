#ifndef LOG_CONFIG_H
#define LOG_CONFIG_H 

/* Standard library includes */
#include <stdint.h>

/** @brief Log config enumeration. */
typedef enum log_config_e
{
	LOG_CONFIG_LOG_LINE_MAXLEN  = 1024, /**< log line max length     */
	LOG_CONFIG_USER_MSG_MAXLEN  = 512   /**< user message max length */
} log_config_e;

/** @brief Log sink options */
typedef enum log_sink_e
{
	LOG_SINK_STDOUT,                  /**< STDOUT sink          */
	LOG_SINK_FILE,                    /**< FILE sink            */
	LOG_SINK_STDOUT_AND_FILE,         /**< STDOUT and FILE sink */
	LOG_SINK_SELECT = LOG_SINK_STDOUT /**< selected log sink    */
} log_sink_e;

/** @brief Log file config enumeration */
typedef enum log_file_config_e
{
	LOG_FILE_CONFIG_PERMISSIONS = 0644 /**< log file permissions */
} log_file_config_e;

/** @brief Log file path */
#define LOG_FILE_PATH "log.txt"

/** @brief Log file mode */
#define LOG_FILE_MODE (O_WRONLY | O_CREAT | O_APPEND)

/**
 * @name Log level options
 * @{
 */
/** @brief Enables all log levels. */
#define LOG_LEVEL_DEBUG   UINT8_C(0)
/** @brief Enables INFO, WARNING and ERROR levels. */
#define LOG_LEVEL_INFO    UINT8_C(1)
/** @brief Enables WARNING and ERROR levels. */
#define LOG_LEVEL_WARNING UINT8_C(2)
/** @brief Enables only ERROR level. */
#define LOG_LEVEL_ERROR   UINT8_C(3)
/** @brief Disables logging completely. */
#define LOG_LEVEL_NONE    UINT8_C(4)
/**@}*/

#ifndef LOG_LEVEL
	/**
	 * @brief Selected log level.
	 * @note  If not defined by the user, it will be defined as 
	 *        LOG_LEVEL_DEBUG
	 */
	#define LOG_LEVEL LOG_LEVEL_WARNING
#endif

#if LOG_LEVEL < LOG_LEVEL_DEBUG || LOG_LEVEL > LOG_LEVEL_NONE
	#error "Unknown log level selected!"
#endif

#endif
