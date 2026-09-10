/*******************************************************************************
 * File: mod_core
 * Project: KernelLoader
 * Created: 2026/5/1
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

#ifndef KERNELLOADER_MOD_CORE_H
#define KERNELLOADER_MOD_CORE_H
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

typedef struct kernel_mod_ops_t kernel_mod_ops_t;
typedef struct kernel_mod_handle_t kernel_mod_handle_t;
typedef struct kernel_mod_info_t kernel_mod_info_t;

typedef struct kernel_mod_handle_t {
    char* private_dir;
    kernel_mod_ops_t* ops;
    void* lib_handle;
} kernel_mod_handle_t;

typedef struct kernel_mod_ops_t {
    void (*init_mod)(kernel_mod_handle_t* handle);
    void (*cleanup_mod)(kernel_mod_handle_t* handle);
    kernel_mod_info_t* (*get_info)();
} kernel_mod_ops_t;

typedef struct kernel_mod_info_t {
    const char *pkg_id; ///< 唯一包名
    int version_code; ///< 版本代码
    int api_version; ///< api版本
    const char *version;
} kernel_mod_info_t;

MOD_API_EXPORT kernel_mod_ops_t* MOD_CALL_CONV create_kernel_mod();

#undef MOD_API_EXPORT
#undef MOD_CALL_CONV

#if __cplusplus
}
#endif
#endif //KERNELLOADER_MOD_CORE_H