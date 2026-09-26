/*******************************************************************************
 * File: logger
 * Project: KernelLoader
 * Created: 2026/5/4
 * Author: eternalfuture-e38299
 * Github: https://github.com/eternalfuture-e38299
 *
 * MIT License
 *
 * Copyright (c) 2026 eternalfuture-e38299
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *******************************************************************************/

#ifndef KERNELLOADER_LOGGER_H
#define KERNELLOADER_LOGGER_H

#if __cplusplus
extern "C" {
#endif

#if defined(_WIN32) || defined(_WIN64)
#ifdef BUILDING_DLL
#define MOD_API_EXPORT __declspec(dllexport)
#else
#define MOD_API_EXPORT __declspec(dllimport)
#endif
#define MOD_CALL_CONV __cdecl
#else
#define MOD_API_EXPORT __attribute__((visibility("default")))
#define MOD_CALL_CONV
#endif

typedef struct mod_logger_ops_t mod_logger_ops_t;

typedef enum mod_log_level_t {
    MOD_LOG_LEVEL_TRACE,     // 最详细的追踪信息
    MOD_LOG_LEVEL_DEBUG,     // 调试信息
    MOD_LOG_LEVEL_INFO,      // 一般信息
    MOD_LOG_LEVEL_WARNING,   // 警告信息
    MOD_LOG_LEVEL_ERROR,     // 错误信息
    MOD_LOG_LEVEL_CRITICAL,  // 严重错误
    MOD_LOG_LEVEL_FATAL      // 致命错误（通常导致程序退出）
} mod_log_level_t;


#ifdef __cplusplus
inline void (MOD_CALL_CONV *mod_logger_write)(mod_log_level_t level, const char* tag, const char* fmt, ...)
    __attribute__((__format__(printf, 3, 4))) = nullptr;
#else
extern void (MOD_CALL_CONV *mod_logger_write)(mod_log_level_t level, const char* tag, const char* fmt, ...)
    __attribute__((__format__(printf, 3, 4)));
#endif


#undef MOD_API_EXPORT
#undef MOD_CALL_CONV
#if __cplusplus
}
#endif
#endif //KERNELLOADER_LOGGER_H
