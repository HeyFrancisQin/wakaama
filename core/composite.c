/*******************************************************************************
 *
 * Copyright (c) 2013, 2014 Intel Corporation and others.
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v2.0
 * and Eclipse Distribution License v1.0 which accompany this distribution.
 *
 * The Eclipse Public License is available at
 *    http://www.eclipse.org/legal/epl-v20.html
 * The Eclipse Distribution License is available at
 *    http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * Contributors:
 *    David Navarro, Intel Corporation - initial API and implementation
 *    Fabien Fleutot - Please refer to git log
 *    Toby Jaffey - Please refer to git log
 *    Bosch Software Innovations GmbH - Please refer to git log
 *    Pascal Rieux - Please refer to git log
 *
 *******************************************************************************/

/*
 Copyright (c) 2013, 2014 Intel Corporation

 Redistribution and use in source and binary forms, with or without modification,
 are permitted provided that the following conditions are met:

     * Redistributions of source code must retain the above copyright notice,
       this list of conditions and the following disclaimer.
     * Redistributions in binary form must reproduce the above copyright notice,
       this list of conditions and the following disclaimer in the documentation
       and/or other materials provided with the distribution.
     * Neither the name of Intel Corporation nor the names of its contributors
       may be used to endorse or promote products derived from this software
       without specific prior written permission.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 THE POSSIBILITY OF SUCH DAMAGE.

 David Navarro <david.navarro@intel.com>

*/

#include "internals.h"
#include "liblwm2m.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ======================== 调试宏定义 ========================
#define COMPOSITE_DEBUG 1

#if COMPOSITE_DEBUG
#define COMPOSITE_TRACE(fmt, ...) \
    do { fprintf(stderr, "[COMPOSITE][%s():%d] ", __func__, __LINE__); \
         fprintf(stderr, fmt "\n", ##__VA_ARGS__); } while(0)
#define COMPOSITE_INFO(fmt, ...) \
    fprintf(stderr, "[INFO][%s():%d] " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)
#define COMPOSITE_ERROR(fmt, ...) \
    fprintf(stderr, "\033[37;41m [ERROR][%s():%d] \033[0m" fmt "\n", __func__, __LINE__, ##__VA_ARGS__)
#else
#define COMPOSITE_TRACE(fmt, ...)
#define COMPOSITE_INFO(fmt, ...)
#define COMPOSITE_ERROR(fmt, ...)
#endif

// ======================== 数据结构定义 ========================

typedef struct
{
    lwm2m_uri_t uri;
    lwm2m_media_type_t format;
    uint8_t *buffer;
    size_t length;
} prv_composite_data_t;

// ======================== 辅助函数 ========================

// 将CBOR数组解码为URI列表
static int prv_decode_uri_list(uint8_t *payload, size_t payload_len,
                               lwm2m_uri_t **uris_out, int *uri_count_out)
{
    COMPOSITE_TRACE("payload=%p, payload_len=%zu", (void*)payload, payload_len);
    COMPOSITE_TRACE("First bytes: 0x%02x 0x%02x 0x%02x", 
                    payload[0], payload_len > 1 ? payload[1] : 0, payload_len > 2 ? payload[2] : 0);
    
    if (payload == NULL || payload_len == 0)
    {
        COMPOSITE_ERROR("Invalid payload: NULL or empty");
        return -1;
    }

    // 初始化输出参数
    *uris_out = NULL;
    *uri_count_out = 0;
    
    uint8_t first_byte = payload[0];
    int uri_count = 0;
    size_t offset = 1;
    uint8_t cbor_type = (first_byte & 0xE0) >> 5;  // 获取CBOR主类型 (0-7)
    
    COMPOSITE_TRACE("CBOR type=%d (0=uint, 1=nint, 2=bstr, 3=tstr, 4=array, 5=map, 6=tag, 7=simple)", cbor_type);
    
    // 处理不同的CBOR类型
    if (cbor_type == 4)  // Array (0x9x)
    {
        // 解析CBOR数组长度
        uint8_t len_type = first_byte & 0x1F;
        
        if (len_type < 24)
        {
            uri_count = len_type;
        }
        else if (len_type == 24)
        {
            if (payload_len < 2)
            {
                COMPOSITE_ERROR("Insufficient data for CBOR array length");
                return -1;
            }
            uri_count = payload[1];
            offset = 2;
        }
        else if (len_type == 25)
        {
            if (payload_len < 3)
            {
                COMPOSITE_ERROR("Insufficient data for CBOR array length");
                return -1;
            }
            uri_count = (payload[1] << 8) | payload[2];
            offset = 3;
        }
        else
        {
            COMPOSITE_ERROR("Unsupported CBOR array length encoding: %d", len_type);
            return -1;
        }
        
        COMPOSITE_TRACE("CBOR array with %d elements, offset=%zu", uri_count, offset);
        COMPOSITE_TRACE("First array element byte: 0x%02x", offset < payload_len ? payload[offset] : 0xFF);
    }
    else if (cbor_type == 5)  // Map (0xax) - SenML CBOR格式
    {
        // 服务器可能发送的是SenML CBOR Map格式，例如：
        // {"bn": "/1/0", "e": [{"n": "/0", "v": 42}, ...]}
        // 或者其他Map结构，这里我们尝试解析它
        
        uint8_t len_type = first_byte & 0x1F;
        int map_count = 0;
        
        if (len_type < 24)
        {
            map_count = len_type;
        }
        else if (len_type == 24)
        {
            if (payload_len < 2)
            {
                COMPOSITE_ERROR("Insufficient data for CBOR map length");
                return -1;
            }
            map_count = payload[1];
            offset = 2;
        }
        else if (len_type == 25)
        {
            if (payload_len < 3)
            {
                COMPOSITE_ERROR("Insufficient data for CBOR map length");
                return -1;
            }
            map_count = (payload[1] << 8) | payload[2];
            offset = 3;
        }
        
        COMPOSITE_TRACE("CBOR map with %d key-value pairs, offset=%zu", map_count, offset);
        
        // 对于Map格式，尝试解析"bn"(baseName)字段作为基础URI
        // 然后可能还有"e"(entries)数组包含相对URI
        
        // 简化处理：如果是Map，我们尝试从整个payload中提取所有可能的URI字符串
        // 这是一个临时的、灵活的方法
        
        // 统计payload中可能的URI个数（包含'/'的字符串）
        uri_count = 0;
        for (size_t i = offset; i < payload_len; i++)
        {
            // 查找CBOR文本字符串标签 (0x6x)
            if ((payload[i] & 0xE0) == 0x60)
            {
                uint8_t str_len_type = payload[i] & 0x1F;
                size_t str_len = 0;
                
                // 简单估计字符串长度（假设长度编码< 24）
                if (str_len_type < 24)
                {
                    str_len = str_len_type;
                    if (i + 1 + str_len <= payload_len)
                    {
                        // 检查字符串是否看起来像URI
                        uint8_t *str_data = &payload[i + 1];
                        if (str_len > 0 && str_data[0] == '/')
                        {
                            uri_count++;
                            i += str_len;  // 跳过已处理的字符串
                        }
                    }
                }
            }
        }
        
        if (uri_count == 0)
        {
            COMPOSITE_ERROR("No URI strings found in CBOR map");
            return -1;
        }
        
        COMPOSITE_TRACE("Found %d potential URIs in CBOR map", uri_count);
    }
    else
    {
        // 不是Array也不是Map，尝试简化的URI字符串列表格式
        COMPOSITE_TRACE("Not CBOR array/map, trying simplified string format (type=%d)", cbor_type);
        
        // 统计URI数量（通过计数斜杠数）
        for (size_t i = 0; i < payload_len; i++)
        {
            if (payload[i] == '/' && (i == 0 || payload[i-1] == '\0'))
            {
                uri_count++;
            }
        }
        
        if (uri_count == 0)
        {
            COMPOSITE_ERROR("No URIs found in payload");
            return -1;
        }
        
        COMPOSITE_TRACE("Found %d URIs in simplified string format", uri_count);
    }
    
    if (uri_count <= 0 || uri_count > 32)
    {
        COMPOSITE_ERROR("Invalid URI count: %d (max 32)", uri_count);
        return -1;
    }
    
    // 分配URI数组
    lwm2m_uri_t *uris = (lwm2m_uri_t *)lwm2m_malloc(sizeof(lwm2m_uri_t) * uri_count);
    if (uris == NULL)
    {
        COMPOSITE_ERROR("Failed to allocate memory for %d URIs", uri_count);
        return -1;
    }
    memset(uris, 0, sizeof(lwm2m_uri_t) * uri_count);
    
    // 解析每个URI
    int parsed_count = 0;
    size_t data_offset = offset;
    
    if (cbor_type == 4)  // Array
    {
        // 处理CBOR数组中的元素
        for (int i = 0; i < uri_count && data_offset < payload_len; i++)
        {
            if (data_offset >= payload_len)
            {
                COMPOSITE_ERROR("Unexpected end of CBOR data at array index %d", i);
                break;
            }
            
            uint8_t elem_header = payload[data_offset];
            uint8_t elem_cbor_type = (elem_header & 0xE0) >> 5;
            
            COMPOSITE_TRACE("Array element %d: header=0x%02x, type=%d", i, elem_header, elem_cbor_type);
            
            // 检查元素类型：可能是文本字符串(3)或Map(5)
            if (elem_cbor_type == 3)  // 文本字符串 (0x6x)
            {
                data_offset++;
                
                // 解析字符串长度
                uint8_t len_type = elem_header & 0x1F;
                size_t str_len = 0;
                
                if (len_type < 24)
                {
                    str_len = len_type;
                }
                else if (len_type == 24)
                {
                    if (data_offset >= payload_len)
                    {
                        COMPOSITE_ERROR("Insufficient data for string length at array index %d", i);
                        break;
                    }
                    str_len = payload[data_offset];
                    data_offset++;
                }
                else if (len_type == 25)
                {
                    if (data_offset + 1 >= payload_len)
                    {
                        COMPOSITE_ERROR("Insufficient data for string length at array index %d", i);
                        break;
                    }
                    str_len = (payload[data_offset] << 8) | payload[data_offset + 1];
                    data_offset += 2;
                }
                else
                {
                    COMPOSITE_ERROR("Unsupported string length encoding: %d", len_type);
                    break;
                }
                
                if (data_offset + str_len > payload_len)
                {
                    COMPOSITE_ERROR("String length %zu exceeds remaining payload", str_len);
                    break;
                }
                
                // 直接解析为URI（这是混合格式中的直接URI元素）
                int result = lwm2m_stringToUri((const char *)&payload[data_offset], str_len, &uris[parsed_count]);
                if (result == 0)
                {
                    COMPOSITE_ERROR("Failed to parse direct URI at array index %d from \"%.*s\"", i, (int)str_len, &payload[data_offset]);
                    break;
                }
                
                COMPOSITE_TRACE("Parsed direct text string URI %d: object=%u instance=%u resource=%u", parsed_count,
                              uris[parsed_count].objectId, uris[parsed_count].instanceId, uris[parsed_count].resourceId);
                
                data_offset += str_len;
                parsed_count++;
            }
            else if (elem_cbor_type == 5)  // Map (0xax) - 元素是Map类型
            {
                COMPOSITE_TRACE("Array element %d is a Map, attempting to extract URI fields", i);
                
                uint8_t map_len_type = elem_header & 0x1F;
                int map_pair_count = 0;
                
                if (map_len_type < 24)
                {
                    map_pair_count = map_len_type;
                }
                else if (map_len_type == 24)
                {
                    if (data_offset + 1 >= payload_len)
                    {
                        COMPOSITE_ERROR("Insufficient data for map length at array index %d", i);
                        break;
                    }
                    map_pair_count = payload[data_offset + 1];
                }
                else
                {
                    COMPOSITE_ERROR("Unsupported map length encoding at array index %d: %d", i, map_len_type);
                    break;
                }
                
                data_offset++;
                if (map_len_type >= 24)
                {
                    data_offset++;
                }
                
                COMPOSITE_TRACE("Array element %d is Map with %d key-value pairs, starting at offset=%zu", i, map_pair_count, data_offset);
                COMPOSITE_TRACE("  Map header was at offset=%zu (size %zu), now at key-value parsing offset=%zu", 
                              data_offset - (map_len_type >= 24 ? 2 : 1), (size_t)(map_len_type >= 24 ? 2 : 1), data_offset);
                
                // 在Map中查找名为"n"(name)的字段，该字段包含URI
                // SenML CBOR中使用整数key，其中 0 = "n" (name/URI)
                int found_uri = 0;
                for (int j = 0; j < map_pair_count && data_offset < payload_len; j++)
                {
                    uint8_t key_header = payload[data_offset];
                    uint8_t key_type = (key_header & 0xE0) >> 5;
                    
                    COMPOSITE_TRACE("Map pair %d: key_header=0x%02x at offset=%zu, key_type=%d", j, key_header, data_offset, key_type);
                    
                    int key_value = -999;  // 用于存储整数key的值
                    int is_name_field = 0;
                    
                    if (key_type == 3)  // 文本字符串key
                    {
                        COMPOSITE_TRACE("Map key %d is text string", j);
                        data_offset++;
                        uint8_t key_len_type = key_header & 0x1F;
                        size_t key_len = 0;
                        
                        if (key_len_type < 24)
                        {
                            key_len = key_len_type;
                        }
                        else if (key_len_type == 24 && data_offset < payload_len)
                        {
                            key_len = payload[data_offset];
                            data_offset++;
                        }
                        else
                        {
                            COMPOSITE_ERROR("Cannot parse map key length at array index %d, pair %d", i, j);
                            break;
                        }
                        
                        if (data_offset + key_len > payload_len)
                        {
                            COMPOSITE_ERROR("Map key exceeds payload at array index %d, pair %d", i, j);
                            break;
                        }
                        
                        // 检查key是否为"n" (URI字段)
                        is_name_field = (key_len == 1 && payload[data_offset] == 'n');
                        data_offset += key_len;
                    }
                    else if (key_type == 0)  // unsigned integer key (SenML方式)
                    {
                        uint8_t key_len_type = key_header & 0x1F;
                        data_offset++;  // 跳过key header
                        
                        if (key_len_type < 24)
                        {
                            key_value = key_len_type;
                        }
                        else if (key_len_type == 24 && data_offset < payload_len)
                        {
                            key_value = payload[data_offset];
                            data_offset++;
                        }
                        else
                        {
                            COMPOSITE_ERROR("Unsupported uint key encoding at array index %d, pair %d", i, j);
                            break;
                        }
                        
                        COMPOSITE_TRACE("Map key %d is uint, value=%d", j, key_value);
                        
                        // SenML中 key=0 表示 "n" (name/URI字段)
                        is_name_field = (key_value == 0);
                    }
                    else if (key_type == 1)  // negative integer key (SenML方式)
                    {
                        uint8_t key_len_type = key_header & 0x1F;
                        data_offset++;  // 跳过key header
                        
                        if (key_len_type < 24)
                        {
                            key_value = -1 - (int)key_len_type;
                        }
                        else if (key_len_type == 24 && data_offset < payload_len)
                        {
                            key_value = -1 - (int)payload[data_offset];
                            data_offset++;
                        }
                        else
                        {
                            COMPOSITE_ERROR("Unsupported nint key encoding at array index %d, pair %d", i, j);
                            break;
                        }
                        
                        COMPOSITE_TRACE("Map key %d is nint, value=%d", j, key_value);
                        
                        // SenML中负数key用于base字段 (-1="bn", -2="bt" 等)
                        // 这里我们只关心正的"name"字段(key=0)
                    }
                    else
                    {
                        COMPOSITE_ERROR("Unsupported map key type at array index %d, pair %d: type=%d (header=0x%02x)", 
                                      i, j, key_type, key_header);
                        break;
                    }
                    
                    // 现在解析value
                    if (data_offset >= payload_len)
                    {
                        COMPOSITE_ERROR("No value for map key at array index %d, pair %d", i, j);
                        break;
                    }
                    
                    uint8_t val_header = payload[data_offset];
                    uint8_t val_type = (val_header & 0xE0) >> 5;
                    
                    if (is_name_field && val_type == 3)  // 如果key是"n"且value是text string
                    {
                        data_offset++;
                        uint8_t val_len_type = val_header & 0x1F;
                        size_t val_len = 0;
                        
                        if (val_len_type < 24)
                        {
                            val_len = val_len_type;
                        }
                        else if (val_len_type == 24 && data_offset < payload_len)
                        {
                            val_len = payload[data_offset];
                            data_offset++;
                        }
                        else
                        {
                            COMPOSITE_ERROR("Cannot parse map value length at array index %d", i);
                            break;
                        }
                        
                        if (data_offset + val_len > payload_len)
                        {
                            COMPOSITE_ERROR("Map value exceeds payload at array index %d", i);
                            break;
                        }
                        
                        // 尝试解析为URI（使用parsed_count作为数组索引）
                        // 如果URI不以'/'开头，需要添加它
                        const uint8_t *uri_data = &payload[data_offset];
                        int uri_needs_slash = (val_len > 0 && uri_data[0] != '/') ? 1 : 0;
                        
                        int result;
                        if (uri_needs_slash)
                        {
                            // 创建临时buffer，添加前导'/'
                            char temp_uri[256];
                            if (val_len + 1 < sizeof(temp_uri))
                            {
                                temp_uri[0] = '/';
                                memcpy(&temp_uri[1], uri_data, val_len);
                                result = lwm2m_stringToUri(temp_uri, val_len + 1, &uris[parsed_count]);
                            }
                            else
                            {
                                result = 0;
                            }
                        }
                        else
                        {
                            result = lwm2m_stringToUri((const char *)uri_data, val_len, &uris[parsed_count]);
                        }
                        
                        if (result != 0)
                        {
                            COMPOSITE_TRACE("Parsed Map URI %d from field 'n': object=%u instance=%u resource=%u", parsed_count,
                                          uris[parsed_count].objectId, uris[parsed_count].instanceId, uris[parsed_count].resourceId);
                            found_uri = 1;
                            parsed_count++;
                        }
                        else
                        {
                            COMPOSITE_ERROR("Failed to parse URI from Map field 'n' at array index %d from \"%.*s\"", i, (int)val_len, &payload[data_offset]);
                        }
                        
                        data_offset += val_len;
                        COMPOSITE_TRACE("    Pair %d: URI parsed, offset after value=%zu", j, data_offset);
                    }
                    else
                    {
                        // 只有key=0 (SenML "n"字段)才是URI
                        // 其他key的value需要跳过
                        COMPOSITE_TRACE("Map[key=%d] is not a URI field, skipping", key_value);
                        
                        // 根据value_type来正确跳过
                        if (val_type == 3)  // text string value
                        {
                            data_offset++;
                            uint8_t v_len_type = val_header & 0x1F;
                            size_t v_len = 0;
                            if (v_len_type < 24)
                            {
                                v_len = v_len_type;
                            }
                            else if (v_len_type == 24 && data_offset < payload_len)
                            {
                                v_len = payload[data_offset];
                                data_offset++;
                            }
                            
                            // 诊断输出：显示跳过的字符串
                            if (data_offset + v_len <= payload_len)
                            {
                                COMPOSITE_TRACE("  Skipping text value: \"%.*s\" (key=%d is not URI field)", 
                                              (int)v_len, &payload[data_offset], key_value);
                                
                                // 作为fallback，也尝试将这个字符串作为URI处理
                                // 以防key=-2是base name，该值可能就是实际的URI
                                const uint8_t *uri_data = &payload[data_offset];
                                int uri_needs_slash = (v_len > 0 && uri_data[0] != '/') ? 1 : 0;
                                
                                int uri_result;
                                if (uri_needs_slash)
                                {
                                    // 创建临时buffer，添加前导'/'
                                    char temp_uri[256];
                                    if (v_len + 1 < sizeof(temp_uri))
                                    {
                                        temp_uri[0] = '/';
                                        memcpy(&temp_uri[1], uri_data, v_len);
                                        uri_result = lwm2m_stringToUri(temp_uri, v_len + 1, &uris[parsed_count]);
                                    }
                                    else
                                    {
                                        uri_result = 0;
                                    }
                                }
                                else
                                {
                                    uri_result = lwm2m_stringToUri((const char *)uri_data, v_len, &uris[parsed_count]);
                                }
                                
                                if (uri_result != 0)
                                {
                                    // 验证解析的URI是否有效（不能全是65535，那表示无效）
                                    int is_valid_uri = !(uris[parsed_count].objectId == 65535 && 
                                                         uris[parsed_count].instanceId == 65535 && 
                                                         uris[parsed_count].resourceId == 65535);
                                    
                                    if (is_valid_uri)
                                    {
                                        COMPOSITE_TRACE("  Fallback: Found VALID URI from key=%d (base field): object=%u instance=%u resource=%u", 
                                                      key_value, uris[parsed_count].objectId, uris[parsed_count].instanceId, uris[parsed_count].resourceId);
                                        found_uri = 1;
                                        parsed_count++;
                                    }
                                    else
                                    {
                                        COMPOSITE_TRACE("  Fallback: Rejected INVALID URI from key=%d (all IDs are 65535)", key_value);
                                    }
                                }
                            }
                            
                            data_offset += v_len;
                            COMPOSITE_TRACE("    Pair %d: Skipped text value, offset after=%zu", j, data_offset);
                        }
                        else if (val_type == 0 || val_type == 1)  // uint or nint
                        {
                            uint8_t v_len_type = val_header & 0x1F;
                            
                            // 打印整数value以便诊断
                            if (v_len_type < 24)
                            {
                                int int_val = (val_type == 0) ? v_len_type : (-1 - (int)v_len_type);
                                COMPOSITE_TRACE("  Skipping int value: %d (key=%d is not URI field)", int_val, key_value);
                            }
                            
                            data_offset++;
                            if (v_len_type >= 24)
                            {
                                if (v_len_type == 24)
                                    data_offset += 1;
                                else if (v_len_type == 25)
                                    data_offset += 2;
                                else if (v_len_type == 26)
                                    data_offset += 4;
                                else if (v_len_type == 27)
                                    data_offset += 8;
                            }
                        }
                        else
                        {
                            COMPOSITE_TRACE("  Skipping value type: %d (key=%d is not URI field)", val_type, key_value);
                            data_offset++;
                        }
                    }
                }
                
                if (!found_uri)
                {
                    COMPOSITE_ERROR("Could not extract URI from Map at array index %d", i);
                    COMPOSITE_TRACE("Next byte offset would be: %zu, hex: 0x%02x", data_offset, 
                                  data_offset < payload_len ? payload[data_offset] : 0xFF);
                    break;
                }
                
                COMPOSITE_TRACE("Successfully parsed URI from Map at array index %d, final offset=%zu (next byte: 0x%02x)", i, data_offset,
                              data_offset < payload_len ? payload[data_offset] : 0xFF);
            }
            else
            {
                COMPOSITE_ERROR("Unsupported array element type at index %d: type=%d (header=0x%02x)", i, elem_cbor_type, elem_header);
                COMPOSITE_TRACE("Skipping this element, offset now at %zu", data_offset);
                
                // 尝试跳过这个元素，以便继续处理后续元素
                // 这可能是数字或其他不支持的类型
                uint8_t elem_len_type = elem_header & 0x1F;
                data_offset++;
                
                if (elem_cbor_type == 0 || elem_cbor_type == 1)  // uint or nint
                {
                    // 整数值可能在header中编码，或在后续字节中
                    if (elem_len_type >= 24)
                    {
                        if (elem_len_type == 24)
                            data_offset += 1;
                        else if (elem_len_type == 25)
                            data_offset += 2;
                        else if (elem_len_type == 26)
                            data_offset += 4;
                        else if (elem_len_type == 27)
                            data_offset += 8;
                    }
                    COMPOSITE_TRACE("Skipped integer element, new offset=%zu", data_offset);
                }
                else if (elem_cbor_type == 2)  // byte string
                {
                    size_t bstr_len = 0;
                    if (elem_len_type < 24)
                    {
                        bstr_len = elem_len_type;
                    }
                    else if (elem_len_type == 24 && data_offset < payload_len)
                    {
                        bstr_len = payload[data_offset];
                        data_offset++;
                    }
                    data_offset += bstr_len;
                    COMPOSITE_TRACE("Skipped byte string element (len=%zu), new offset=%zu", bstr_len, data_offset);
                }
                else if (elem_cbor_type == 6)  // tagged value
                {
                    // tag值可能在header中或后续字节
                    if (elem_len_type >= 24)
                    {
                        if (elem_len_type == 24)
                            data_offset += 1;
                        else if (elem_len_type == 25)
                            data_offset += 2;
                        else if (elem_len_type == 26)
                            data_offset += 4;
                        else if (elem_len_type == 27)
                            data_offset += 8;
                    }
                    COMPOSITE_TRACE("Skipped tagged element, new offset=%zu", data_offset);
                }
                else
                {
                    COMPOSITE_TRACE("Unknown element type %d, cannot skip reliably", elem_cbor_type);
                }
                
                // 不break，继续尝试处理下一个元素
                continue;
            }
        }
    }
    else if (cbor_type == 5)  // Map
    {
        // 处理CBOR Map中的文本字符串（尝试提取所有看起来像URI的字符串）
        int found_uris = 0;
        
        for (size_t i = offset; i < payload_len && found_uris < uri_count; i++)
        {
            // 查找CBOR文本字符串
            if ((payload[i] & 0xE0) == 0x60)
            {
                uint8_t len_type = payload[i] & 0x1F;
                size_t str_len = 0;
                size_t str_offset = i + 1;
                
                if (len_type < 24)
                {
                    str_len = len_type;
                }
                else if (len_type == 24 && i + 1 < payload_len)
                {
                    str_len = payload[i + 1];
                    str_offset = i + 2;
                }
                else if (len_type == 25 && i + 2 < payload_len)
                {
                    str_len = (payload[i + 1] << 8) | payload[i + 2];
                    str_offset = i + 3;
                }
                else
                {
                    i++;
                    continue;
                }
                
                if (str_offset + str_len > payload_len)
                {
                    break;
                }
                
                // 检查是否看起来像URI（以'/'开头）
                if (str_len > 0 && payload[str_offset] == '/')
                {
                    int result = lwm2m_stringToUri((const char *)&payload[str_offset], str_len, &uris[found_uris]);
                    if (result != 0)
                    {
                        COMPOSITE_TRACE("Parsed URI %d from map: object=%u instance=%u resource=%u", found_uris,
                                      uris[found_uris].objectId, uris[found_uris].instanceId, uris[found_uris].resourceId);
                        found_uris++;
                        parsed_count++;
                    }
                }
                
                i = str_offset + str_len - 1;  // 跳过已处理的字符串
            }
        }
    }
    else  // 简化格式
    {
        // 处理简化的字符串列表格式
        for (int i = 0; i < uri_count && data_offset < payload_len; i++)
        {
            size_t str_start = data_offset;
            size_t str_len = 0;
            
            // 查找下一个分隔符或结束
            while (data_offset < payload_len && payload[data_offset] != '\0')
            {
                data_offset++;
            }
            str_len = data_offset - str_start;
            
            if (str_len > 0)
            {
                int result = lwm2m_stringToUri((const char *)&payload[str_start], str_len, &uris[i]);
                if (result == 0)
                {
                    COMPOSITE_ERROR("Failed to parse URI at index %d", i);
                    break;
                }
                
                COMPOSITE_TRACE("Parsed URI %d: object=%u instance=%u resource=%u", i,
                              uris[i].objectId, uris[i].instanceId, uris[i].resourceId);
                parsed_count++;
            }
            
            // 跳过分隔符
            if (data_offset < payload_len && payload[data_offset] == '\0')
            {
                data_offset++;
            }
        }
    }
    
    if (parsed_count == 0)
    {
        COMPOSITE_ERROR("Failed to parse any URIs from payload");
        lwm2m_free(uris);
        return -1;
    }
    
    *uris_out = uris;
    *uri_count_out = parsed_count;
    
    COMPOSITE_TRACE("Successfully decoded %d/%d URIs", parsed_count, uri_count);
    return 0;
}

// 将多个资源的值打包为CBOR格式（SenML CBOR编码）
static int prv_encode_multi_resource(lwm2m_uri_t *uris, int uriCount,
                                     lwm2m_context_t *contextP,
                                     uint8_t **payload_out, size_t *payload_len_out)
{
    COMPOSITE_TRACE("=== ENCODE MULTI RESOURCE START === uriCount=%d", uriCount);
    
    if (payload_out == NULL || payload_len_out == NULL)
    {
        COMPOSITE_ERROR("Invalid output parameters");
        return COAP_400_BAD_REQUEST;
    }
    
    *payload_out = NULL;
    *payload_len_out = 0;
    
    // 使用框架的 object_readCompositeData() 一次性读取所有URI的数据
    // 这个函数会返回一个正确结构化的数据数组
    lwm2m_data_t *dataArray = NULL;
    int dataSize = 0;
    
    COMPOSITE_TRACE("Reading composite data for %d URIs", uriCount);
    uint8_t readResult = object_readCompositeData(contextP, uris, uriCount, &dataSize, &dataArray);
    
    if (readResult != COAP_205_CONTENT || dataArray == NULL || dataSize <= 0)
    {
        COMPOSITE_ERROR("Failed to read composite data: result=%d, dataSize=%d", readResult, dataSize);
        if (dataArray != NULL)
        {
            lwm2m_data_free(dataSize, dataArray);
        }
        return COAP_404_NOT_FOUND;
    }
    
    COMPOSITE_TRACE("Successfully read %d data items", dataSize);
    
    // 直接调用一次 senml_cbor_serialize() 来编码所有数据
    // 这样框架会自动生成正确的CBOR数组，每个记录都有完整的URI
    int encodeResult = senml_cbor_serialize(NULL, dataSize, dataArray, payload_out);
    
    if (encodeResult <= 0)
    {
        COMPOSITE_ERROR("Failed to encode composite data: result=%d", encodeResult);
        lwm2m_data_free(dataSize, dataArray);
        return COAP_500_INTERNAL_SERVER_ERROR;
    }
    
    *payload_len_out = encodeResult;
    
    COMPOSITE_TRACE("Successfully encoded %zu bytes", *payload_len_out);
    
    // 清理数据数组
    lwm2m_data_free(dataSize, dataArray);
    
    COMPOSITE_TRACE("=== ENCODE MULTI RESOURCE DONE === payload_len=%zu", *payload_len_out);
    return COAP_205_CONTENT;
}

// ======================== Composite Read ========================

uint8_t composite_read(lwm2m_context_t *contextP,
                       lwm2m_uri_t *uriP,
                       lwm2m_server_t *serverP,
                       coap_packet_t *message,
                       coap_packet_t *response)
{
    COMPOSITE_TRACE("=== COMPOSITE READ START ===");
    COMPOSITE_TRACE("contextP=%p, uriP=%p, serverP=%p", (void*)contextP, (void*)uriP, (void*)serverP);
    COMPOSITE_TRACE("message->payload_len=%zu, message->code=%d", 
                    message->payload_len, message->code);
    
    lwm2m_uri_t *uris = NULL;
    int uriCount = 0;
    uint8_t result;

    // 解析请求负载中的URI列表
    COMPOSITE_TRACE("Decoding URI list from payload");
    if (prv_decode_uri_list(message->payload, message->payload_len, &uris, &uriCount) != 0)
    {
        COMPOSITE_ERROR("Failed to decode URI list");
        return COAP_400_BAD_REQUEST;
    }

    // 如果没有URI，返回错误
    if (uriCount == 0 || uris == NULL)
    {
        COMPOSITE_ERROR("No URIs decoded or uriCount=0");
        return COAP_400_BAD_REQUEST;
    }
    
    COMPOSITE_TRACE("Successfully decoded %d URIs", uriCount);

    // 编码多个资源的值为CBOR格式
    uint8_t *payload = NULL;
    size_t payload_len = 0;
    COMPOSITE_TRACE("Encoding resources to CBOR format");
    result = prv_encode_multi_resource(uris, uriCount, contextP, &payload, &payload_len);

    // 设置响应内容类型和负载
    COMPOSITE_TRACE("Setting response: content_type=%d, payload_len=%zu", 
                    LWM2M_CONTENT_SENML_CBOR, payload_len);
    coap_set_header_content_type(response, LWM2M_CONTENT_SENML_CBOR);
    
    // 注意：coap_set_payload() 不会复制buffer，而是直接使用指针
    // CoAP层会负责释放这个payload，所以这里不应该释放它
    coap_set_payload(response, payload, payload_len);

    // 清理资源：只释放URI数组，不释放payload（由CoAP层管理）
    if (uris != NULL)
    {
        lwm2m_free(uris);
    }
    // 注意：不要释放payload，因为CoAP响应对象现在拥有它

    COMPOSITE_TRACE("=== COMPOSITE READ DONE === result=%d", result);
    return result;
}

// ======================== Composite Write ========================

uint8_t composite_write(lwm2m_context_t *contextP,
                        lwm2m_uri_t *uriP,
                        lwm2m_server_t *serverP,
                        coap_packet_t *message,
                        coap_packet_t *response)
{
    COMPOSITE_TRACE("=== COMPOSITE WRITE START ===");
    COMPOSITE_TRACE("contextP=%p, uriP=%p, serverP=%p", (void*)contextP, (void*)uriP, (void*)serverP);
    COMPOSITE_TRACE("message->payload_len=%zu, message->code=%d", 
                    message->payload_len, message->code);
    
    lwm2m_data_t *dataP = NULL;
    int dataSize = 0;
    int parseResult = 0;
    uint8_t result = COAP_204_CHANGED;
    int successCount = 0;
    int failureCount = 0;

    // 检查请求包含了必要的内容
    if (message->payload == NULL || message->payload_len == 0)
    {
        COMPOSITE_ERROR("Invalid payload: NULL or empty");
        return COAP_400_BAD_REQUEST;
    }

    // 获取内容类型，用于解析负载
    uint16_t contentType = message->content_type;
    COMPOSITE_TRACE("Content-Type: %d", contentType);
    if (contentType == 0)
    {
        // 如果没有指定内容类型，使用默认的CBOR
        contentType = LWM2M_CONTENT_SENML_CBOR;
    }

    // 将内容类型转换为媒体类型枚举值
    lwm2m_media_type_t format = LWM2M_CONTENT_SENML_CBOR;
    switch (contentType)
    {
    case LWM2M_CONTENT_SENML_CBOR:
        format = LWM2M_CONTENT_SENML_CBOR;
        COMPOSITE_TRACE("Using SenML CBOR format");
        break;
#ifdef LWM2M_SUPPORT_SENML_JSON
    case LWM2M_CONTENT_SENML_JSON:
        format = LWM2M_CONTENT_SENML_JSON;
        COMPOSITE_TRACE("Using SenML JSON format");
        break;
#endif
#ifdef LWM2M_SUPPORT_TLV
    case LWM2M_CONTENT_TLV:
        format = LWM2M_CONTENT_TLV;
        COMPOSITE_TRACE("Using TLV format");
        break;
#endif
#ifdef LWM2M_SUPPORT_JSON
    case LWM2M_CONTENT_JSON:
        format = LWM2M_CONTENT_JSON;
        COMPOSITE_TRACE("Using JSON format");
        break;
#endif
    default:
        format = LWM2M_CONTENT_SENML_CBOR;
        COMPOSITE_TRACE("Unknown content type %d, using SenML CBOR", contentType);
        break;
    }

    // 对于 Composite Write，负载中包含完整的 URI 和数据
    // SenML JSON/CBOR 格式会在 "bn" (base name) 字段中包含基础 URI，
    // "n" (name) 字段包含相对 URI，完整 URI = bn + n
    // 
    // 根据 lwm2m_data_parse() 的实现，当传递 NULL uriP 时，
    // SenML 解析器能够从负载本身的结构中提取完整的 URI 信息
    
    COMPOSITE_TRACE("Parsing payload with format=%d (using NULL baseUri for SenML auto-detection)", format);
    
    // 对于 Composite Write，使用 NULL 作为基础 URI，让解析器从负载中自动检测
    // 这样 SenML JSON 中的 "bn" 字段可以被正确解析
    parseResult = lwm2m_data_parse(NULL, message->payload, message->payload_len, format, &dataP);
    
    if (parseResult <= 0)
    {
        COMPOSITE_ERROR("Failed to parse payload with format %d, result=%d", format, parseResult);
        COMPOSITE_TRACE("Payload size=%zu, first bytes: 0x%02x 0x%02x 0x%02x",
                       message->payload_len,
                       message->payload_len > 0 ? message->payload[0] : 0xFF,
                       message->payload_len > 1 ? message->payload[1] : 0xFF,
                       message->payload_len > 2 ? message->payload[2] : 0xFF);
        // 如果 NULL 失败，尝试用根 URI 作为备选方案
        COMPOSITE_TRACE("Retrying with root URI as base");
        lwm2m_uri_t rootUri;
        memset(&rootUri, 0, sizeof(rootUri));
        parseResult = lwm2m_data_parse(&rootUri, message->payload, message->payload_len, format, &dataP);
        if (parseResult <= 0)
        {
            COMPOSITE_ERROR("Failed to parse payload even with root URI fallback, result=%d", parseResult);
            return COAP_400_BAD_REQUEST;
        }
    }

    dataSize = parseResult;
    COMPOSITE_TRACE("Parsed %d data items from payload", dataSize);
    
    if (dataP == NULL || dataSize <= 0)
    {
        COMPOSITE_ERROR("No data parsed from payload");
        return COAP_400_BAD_REQUEST;
    }

    // 遍历所有解析的数据项，为每一项调用 object_writeInstance()
    // 由于我们已经有了解析好的数据结构，直接使用 object_writeInstance() 而不是 object_write()
    // object_write() 期望序列化的二进制缓冲区，但我们有 lwm2m_data_t 结构
    for (int i = 0; i < dataSize; i++)
    {
        COMPOSITE_TRACE("Processing data item %d: type=%d, id=%u", i, dataP[i].type, dataP[i].id);
        
        // 根据数据类型处理
        if (dataP[i].type == LWM2M_TYPE_OBJECT)
        {
            // 这是一个对象，其子项是实例
            COMPOSITE_TRACE("  Object %u with %zu instances", dataP[i].id, dataP[i].value.asChildren.count);
            
            lwm2m_data_t *instanceArray = dataP[i].value.asChildren.array;
            int instanceCount = dataP[i].value.asChildren.count;
            
            for (int j = 0; j < instanceCount; j++)
            {
                lwm2m_uri_t instanceUri;
                memset(&instanceUri, 0, sizeof(instanceUri));
                instanceUri.objectId = dataP[i].id;
                instanceUri.instanceId = instanceArray[j].id;
                
                COMPOSITE_TRACE("    Instance %u with %zu resources", instanceUri.instanceId, 
                              instanceArray[j].value.asChildren.count);
            #if 0
                // 调试：打印资源列表
                if (instanceArray[j].type == LWM2M_TYPE_OBJECT_INSTANCE && 
                    instanceArray[j].value.asChildren.count > 0)
                {
                    for (size_t k = 0; k < instanceArray[j].value.asChildren.count; k++)
                    {
                        lwm2m_data_t *resItem = &instanceArray[j].value.asChildren.array[k];
                        COMPOSITE_TRACE("      Resource %u (type=%d)", resItem->id, resItem->type);
                    }
                }
                else
                {
                    COMPOSITE_ERROR("      Instance %u has unexpected type=%d (expected %d for OBJECT_INSTANCE)",
                                  instanceArray[j].id, instanceArray[j].type, LWM2M_TYPE_OBJECT_INSTANCE);
                }
            #endif
                
                // 使用 object_writeInstance() 来写入实例的所有资源
                // object_writeInstance() 期望 dataP 指向实例数据
                COMPOSITE_TRACE("      Calling object_writeInstance with instanceUri={objectId=%u, instanceId=%u}, instanceArray[j]={id=%u, type=%d, count=%zu}",
                              instanceUri.objectId, instanceUri.instanceId,
                              instanceArray[j].id, instanceArray[j].type, instanceArray[j].value.asChildren.count);
                uint8_t writeResult = object_writeInstance(contextP, &instanceUri, &instanceArray[j]);
                
                if (writeResult == COAP_204_CHANGED)
                {
                    successCount++;
                    COMPOSITE_TRACE("      Write successful for /%u/%u", 
                                  instanceUri.objectId, instanceUri.instanceId);
                }
                else
                {
                    failureCount++;
                    COMPOSITE_ERROR("      Write failed for /%u/%u: result=%d", 
                                  instanceUri.objectId, instanceUri.instanceId, writeResult);
                    if (result == COAP_204_CHANGED)
                    {
                        result = writeResult;
                    }
                }
            }
        }
        else if (dataP[i].type == LWM2M_TYPE_OBJECT_INSTANCE)
        {
            // 这是一个直接的实例（没有对象包装）
            // 这在 SenML 格式中可能发生，例如当只指定 /3/0 时
            COMPOSITE_TRACE("  Direct instance with %zu resources", dataP[i].value.asChildren.count);
            
            lwm2m_uri_t instanceUri;
            memset(&instanceUri, 0, sizeof(instanceUri));
            // 注意：这种情况下 object ID 必须从其他地方获取
            // 但由于我们没有对象 ID，这通常表示解析有问题
            // 尝试从第一个资源中推断
            if (dataP[i].value.asChildren.count > 0)
            {
                instanceUri.objectId = dataP[i].value.asChildren.array[0].id;
                instanceUri.instanceId = dataP[i].id;
                
                COMPOSITE_TRACE("      Inferred objectId=%u from first resource id", instanceUri.objectId);
                
            #if 0
                // 调试：打印资源列表
                for (size_t k = 0; k < dataP[i].value.asChildren.count; k++)
                {
                    lwm2m_data_t *resItem = &dataP[i].value.asChildren.array[k];
                    COMPOSITE_TRACE("      Resource %u (type=%d)", resItem->id, resItem->type);
                }
            #endif
                
                uint8_t writeResult = object_writeInstance(contextP, &instanceUri, &dataP[i]);
                
                if (writeResult == COAP_204_CHANGED)
                {
                    successCount++;
                    COMPOSITE_TRACE("    Write successful for /%u/%u", 
                                  instanceUri.objectId, instanceUri.instanceId);
                }
                else
                {
                    failureCount++;
                    COMPOSITE_ERROR("    Write failed for /%u/%u: result=%d", 
                                  instanceUri.objectId, instanceUri.instanceId, writeResult);
                    if (result == COAP_204_CHANGED)
                    {
                        result = writeResult;
                    }
                }
            }
        }
        else
        {
            COMPOSITE_ERROR("Unexpected data type at index %d: type=%d (expected OBJECT or OBJECT_INSTANCE)",
                          i, dataP[i].type);
        }
    }

    COMPOSITE_TRACE("Composite write completed: %d successful, %d failed", successCount, failureCount);
    
    // 设置响应内容类型
    if (response != NULL)
    {
        coap_set_header_content_type(response, LWM2M_CONTENT_SENML_CBOR);
    }
    
    // 清理解析的数据
    if (dataP != NULL)
    {
        lwm2m_data_free(dataSize, dataP);
    }
    
    // RFC 8810: Composite Write 响应码的规则
    // - 如果所有资源都成功写入 -> 2.04 Changed
    // - 如果部分资源成功 -> 2.04 Changed（部分成功的写入）
    // - 如果没有资源成功 -> 返回第一个错误码（通常是 4.04 Not Found）
    
    COMPOSITE_TRACE("Composite write results: %d successful, %d failed", successCount, failureCount);
    
    if (successCount > 0)
    {
        // 至少有一个资源成功写入
        return COAP_204_CHANGED;
    }
    else if (failureCount > 0)
    {
        // 所有资源都失败，返回记录的第一个错误码
        return result;
    }
    else
    {
        // 没有处理任何资源
        COMPOSITE_ERROR("No resources were processed");
        return COAP_400_BAD_REQUEST;
    }
}

// ======================== Composite Observe ========================

// ======================== 复合观察数据结构 ========================

/**
 * 前向声明
 */
typedef struct prv_composite_observed_ prv_composite_observed_t;

/**
 * 复合观察的观察者（对应于 observe.c 中的 lwm2m_watcher_t）
 * 每个复合观察对应一个观察者，保存该观察的所有状态
 */
typedef struct prv_composite_watcher_
{
    struct prv_composite_watcher_ *next;
    
    // Token 用于识别这个观察
    uint8_t token[8];
    int token_len;
    
    // 对应的服务器
    lwm2m_server_t *server;
    
    // 所有被观察的 URI 列表
    lwm2m_uri_t *uriList;
    int uriCount;
    
    // 观察参数
    lwm2m_attributes_t *parameters;
    
    // 观察状态
    bool active;           // 是否活跃
    time_t lastTime;       // 上次发送通知的时间
    uint16_t lastMid;      // 上次 MID
    uint32_t counter;      // 观察计数器（递增）
    
    // 用于检测变化（缓存上一次的值）
    lwm2m_data_t *lastDataArray;
    int lastDataSize;
    
} prv_composite_watcher_t;

/**
 * 复合观察对象（类似于 observe.c 中的 lwm2m_observed_t）
 * 表示某个资源被观察，可能有多个观察者（watcher）
 */
typedef struct prv_composite_observed_
{
    // 被观察的 URI 列表（主键）
    lwm2m_uri_t *uriList;
    int uriCount;
    
    // 观察者列表
    prv_composite_watcher_t *watcherList;
    
    // 用于链表
    struct prv_composite_observed_ *next;
} prv_composite_observed_t;

// 全局复合观察列表
static prv_composite_observed_t *g_composite_observedList = NULL;

// ======================== 辅助函数 ========================

/**
 * 比较两个 URI 列表是否相等
 */
static bool prv_compareUriLists(lwm2m_uri_t *list1, int count1, 
                                lwm2m_uri_t *list2, int count2)
{
    if (count1 != count2) return false;
    
    for (int i = 0; i < count1; i++)
    {
        if (list1[i].objectId != list2[i].objectId ||
            list1[i].instanceId != list2[i].instanceId ||
            list1[i].resourceId != list2[i].resourceId)
        {
            return false;
        }
    }
    return true;
}

/**
 * 查找已观察的对象
 */
static prv_composite_observed_t* prv_findObserved(lwm2m_uri_t *uriList, int uriCount)
{
    prv_composite_observed_t *targetP = g_composite_observedList;
    
    while (targetP != NULL)
    {
        if (prv_compareUriLists(targetP->uriList, targetP->uriCount, uriList, uriCount))
        {
            return targetP;
        }
        targetP = targetP->next;
    }
    
    return NULL;
}

/**
 * 从链表中移除已观察对象
 */
static void prv_unlinkObserved(prv_composite_observed_t *observedP)
{
    if (g_composite_observedList == observedP)
    {
        g_composite_observedList = g_composite_observedList->next;
    }
    else
    {
        prv_composite_observed_t *parentP = g_composite_observedList;
        
        while (parentP->next != NULL && parentP->next != observedP)
        {
            parentP = parentP->next;
        }
        
        if (parentP->next != NULL)
        {
            parentP->next = parentP->next->next;
        }
    }
}

/**
 * 查找观察者
 */
static prv_composite_watcher_t* prv_findWatcher(prv_composite_observed_t *observedP,
                                                lwm2m_server_t *server)
{
    prv_composite_watcher_t *targetP = observedP->watcherList;
    
    while (targetP != NULL && targetP->server != server)
    {
        targetP = targetP->next;
    }
    
    return targetP;
}

/**
 * 获取或创建观察者
 */
static prv_composite_watcher_t* prv_getWatcher(lwm2m_uri_t *uriList, int uriCount,
                                               lwm2m_server_t *server)
{
    prv_composite_observed_t *observedP;
    prv_composite_watcher_t *watcherP;
    bool allocatedObserver = false;
    
    observedP = prv_findObserved(uriList, uriCount);
    if (observedP == NULL)
    {
        // 创建新的观察对象
        observedP = (prv_composite_observed_t *)lwm2m_malloc(sizeof(prv_composite_observed_t));
        if (observedP == NULL)
        {
            COMPOSITE_ERROR("Failed to allocate observed object");
            return NULL;
        }
        
        memset(observedP, 0, sizeof(prv_composite_observed_t));
        
        // 复制 URI 列表
        observedP->uriList = (lwm2m_uri_t *)lwm2m_malloc(sizeof(lwm2m_uri_t) * uriCount);
        if (observedP->uriList == NULL)
        {
            lwm2m_free(observedP);
            COMPOSITE_ERROR("Failed to allocate URI list");
            return NULL;
        }
        
        memcpy(observedP->uriList, uriList, sizeof(lwm2m_uri_t) * uriCount);
        observedP->uriCount = uriCount;
        
        // 加入全局列表
        observedP->next = g_composite_observedList;
        g_composite_observedList = observedP;
        
        allocatedObserver = true;
        COMPOSITE_TRACE("Created new composite observed object with %d URIs", uriCount);
    }
    
    // 查找或创建观察者
    watcherP = prv_findWatcher(observedP, server);
    if (watcherP == NULL)
    {
        watcherP = (prv_composite_watcher_t *)lwm2m_malloc(sizeof(prv_composite_watcher_t));
        if (watcherP == NULL)
        {
            if (allocatedObserver)
            {
                lwm2m_free(observedP->uriList);
                lwm2m_free(observedP);
            }
            COMPOSITE_ERROR("Failed to allocate watcher");
            return NULL;
        }
        
        memset(watcherP, 0, sizeof(prv_composite_watcher_t));
        watcherP->active = false;
        watcherP->server = server;
        watcherP->counter = 0;
        
        // 加入观察者列表
        watcherP->next = observedP->watcherList;
        observedP->watcherList = watcherP;
        
        COMPOSITE_TRACE("Created new watcher for server %p", (void*)server);
    }
    
    return watcherP;
}

// 注册复合观察
uint8_t composite_observe(lwm2m_context_t *contextP,
                          lwm2m_uri_t *uriP,
                          lwm2m_server_t *serverP,
                          coap_packet_t *message,
                          coap_packet_t *response)
{
    COMPOSITE_TRACE("=== COMPOSITE OBSERVE START ===");
    COMPOSITE_TRACE("contextP=%p, serverP=%p", (void*)contextP, (void*)serverP);
    
    lwm2m_uri_t *uris = NULL;
    int uriCount = 0;
    prv_composite_watcher_t *watcherP = NULL;
    uint8_t result = COAP_205_CONTENT;
    
    // 参数检查
    if (contextP == NULL || serverP == NULL || message == NULL || response == NULL)
    {
        COMPOSITE_ERROR("Invalid parameters");
        return COAP_400_BAD_REQUEST;
    }
    
    if (message->token_len == 0 || message->token_len > 8)
    {
        COMPOSITE_ERROR("Invalid token length: %d", message->token_len);
        return COAP_400_BAD_REQUEST;
    }
    
    if (message->payload == NULL || message->payload_len == 0)
    {
        COMPOSITE_ERROR("Invalid payload: NULL or empty");
        return COAP_400_BAD_REQUEST;
    }
    
    // 解析 URI 列表
    COMPOSITE_TRACE("Decoding URI list from payload");
    if (prv_decode_uri_list(message->payload, message->payload_len, &uris, &uriCount) != 0)
    {
        COMPOSITE_ERROR("Failed to decode URI list");
        return COAP_400_BAD_REQUEST;
    }
    
    if (uriCount == 0 || uris == NULL)
    {
        COMPOSITE_ERROR("No URIs decoded");
        return COAP_400_BAD_REQUEST;
    }
    
    COMPOSITE_TRACE("Decoded %d URIs for observation", uriCount);
    
    // 获取或创建观察者
    watcherP = prv_getWatcher(uris, uriCount, serverP);
    if (watcherP == NULL)
    {
        COMPOSITE_ERROR("Failed to get watcher");
        lwm2m_free(uris);
        return COAP_500_INTERNAL_SERVER_ERROR;
    }
    
    // 保存观察参数
    watcherP->token_len = message->token_len;
    memcpy(watcherP->token, message->token, message->token_len);
    watcherP->active = true;
    watcherP->lastTime = lwm2m_gettime();
    watcherP->lastMid = response->mid;
    watcherP->counter = 0;
    
    COMPOSITE_TRACE("Watcher registered: token_len=%d, active=true", watcherP->token_len);
    
    // 读取初始数据并编码响应
    uint8_t *payload = NULL;
    size_t payloadLen = 0;
    
    result = prv_encode_multi_resource(uris, uriCount, contextP, &payload, &payloadLen);
    
    if (result != COAP_205_CONTENT || payload == NULL || payloadLen == 0)
    {
        COMPOSITE_ERROR("Failed to encode initial data: result=%d", result);
        lwm2m_free(payload);
        lwm2m_free(uris);
        return COAP_500_INTERNAL_SERVER_ERROR;
    }
    
    COMPOSITE_TRACE("Initial data encoded: %zu bytes", payloadLen);
    
    // 缓存初始数据用于变化检测
    int dataSize = 0;
    lwm2m_data_t *dataP = NULL;
    
    if (object_readCompositeData(contextP, uris, uriCount, &dataSize, &dataP) == COAP_205_CONTENT)
    {
        if (watcherP->lastDataArray != NULL)
        {
            lwm2m_data_free(watcherP->lastDataSize, watcherP->lastDataArray);
        }
        watcherP->lastDataArray = dataP;
        watcherP->lastDataSize = dataSize;
    }
    
    // 设置响应
    if (NULL == watcherP->parameters)
    {
        watcherP->parameters = (lwm2m_attributes_t *)lwm2m_malloc(sizeof(lwm2m_attributes_t));
        if (watcherP->parameters == NULL) return COAP_500_INTERNAL_SERVER_ERROR;
        watcherP->parameters->toSet |= LWM2M_ATTR_FLAG_MIN_PERIOD;
        watcherP->parameters->minPeriod = 10;
        watcherP->parameters->toSet |= LWM2M_ATTR_FLAG_MAX_PERIOD;
        watcherP->parameters->maxPeriod = 60;
    }

    coap_set_header_content_type(response, LWM2M_CONTENT_SENML_CBOR);
    coap_set_payload(response, payload, payloadLen);
    coap_set_header_observe(response, watcherP->counter);
    
    COMPOSITE_TRACE("=== COMPOSITE OBSERVE DONE === response_observe=%u", watcherP->counter);
    
    // 注意：不释放 uris，因为它被保存在 observed 对象中
    // 不释放 payload，因为它由 CoAP 层管理
    
    return COAP_205_CONTENT;
}

// ======================== Composite Cancel Observe ========================

uint8_t composite_cancel_observe(lwm2m_context_t *contextP,
                                 lwm2m_uri_t *uriP,
                                 lwm2m_server_t *serverP,
                                 coap_packet_t *message,
                                 coap_packet_t *response)
{
    COMPOSITE_TRACE("=== COMPOSITE CANCEL OBSERVE START ===");
    COMPOSITE_TRACE("contextP=%p, serverP=%p", (void*)contextP, (void*)serverP);
    
    lwm2m_uri_t *uris = NULL;
    int uriCount = 0;
    uint8_t result = COAP_205_CONTENT;
    int canceledCount = 0;
    
    (void)uriP;  // 未使用
    
    if (contextP == NULL || serverP == NULL || message == NULL)
    {
        COMPOSITE_ERROR("Invalid parameters");
        return COAP_400_BAD_REQUEST;
    }
    
    // 如果有 payload，取消特定的观察；否则取消该服务器的所有观察
    if (message->payload != NULL && message->payload_len > 0)
    {
        COMPOSITE_TRACE("Payload present: %zu bytes", message->payload_len);
        
        if (prv_decode_uri_list(message->payload, message->payload_len, &uris, &uriCount) != 0)
        {
            COMPOSITE_ERROR("Failed to decode URI list");
            return COAP_400_BAD_REQUEST;
        }
        
        if (uriCount == 0 || uris == NULL)
        {
            COMPOSITE_ERROR("No URIs decoded");
            return COAP_400_BAD_REQUEST;
        }
        
        COMPOSITE_TRACE("Decoded %d URIs for targeted cancellation", uriCount);
        
        // 查找并取消这些 URI 的观察
        prv_composite_observed_t *observedP = prv_findObserved(uris, uriCount);
        if (observedP != NULL)
        {
            prv_composite_watcher_t *watcherP = prv_findWatcher(observedP, serverP);
            if (watcherP != NULL)
            {
                COMPOSITE_TRACE("Found watcher for targeted cancellation");
                
                // 释放缓存的数据
                if (watcherP->lastDataArray != NULL)
                {
                    lwm2m_data_free(watcherP->lastDataSize, watcherP->lastDataArray);
                    watcherP->lastDataArray = NULL;
                }
                
                // 从观察者列表中移除
                if (observedP->watcherList == watcherP)
                {
                    observedP->watcherList = watcherP->next;
                }
                else
                {
                    prv_composite_watcher_t *parentP = observedP->watcherList;
                    while (parentP->next != NULL && parentP->next != watcherP)
                    {
                        parentP = parentP->next;
                    }
                    if (parentP->next != NULL)
                    {
                        parentP->next = parentP->next->next;
                    }
                }
                
                lwm2m_free(watcherP);
                canceledCount++;
                
                COMPOSITE_TRACE("Watcher removed, checking if observed object is empty");
                
                // 如果观察者列表为空，移除整个观察对象
                if (observedP->watcherList == NULL)
                {
                    COMPOSITE_TRACE("No more watchers, removing observed object");
                    prv_unlinkObserved(observedP);
                    lwm2m_free(observedP->uriList);
                    lwm2m_free(observedP);
                }
            }
        }
        
        lwm2m_free(uris);
    }
    else
    {
        // 没有 payload：取消该服务器的所有观察
        COMPOSITE_TRACE("No payload: cancelling all observations for server %p", (void*)serverP);
        
        prv_composite_observed_t *observedP = g_composite_observedList;
        while (observedP != NULL)
        {
            prv_composite_observed_t *nextObserved = observedP->next;
            prv_composite_watcher_t *watcherP = observedP->watcherList;
            
            while (watcherP != NULL)
            {
                prv_composite_watcher_t *nextWatcher = watcherP->next;
                
                if (watcherP->server == serverP)
                {
                    COMPOSITE_TRACE("Removing watcher for observed object");
                    
                    // 释放缓存的数据
                    if (watcherP->lastDataArray != NULL)
                    {
                        lwm2m_data_free(watcherP->lastDataSize, watcherP->lastDataArray);
                        watcherP->lastDataArray = NULL;
                    }
                    
                    // 从列表中移除
                    if (observedP->watcherList == watcherP)
                    {
                        observedP->watcherList = watcherP->next;
                    }
                    else
                    {
                        prv_composite_watcher_t *parentW = observedP->watcherList;
                        while (parentW->next != NULL && parentW->next != watcherP)
                        {
                            parentW = parentW->next;
                        }
                        if (parentW->next != NULL)
                        {
                            parentW->next = parentW->next->next;
                        }
                    }
                    
                    lwm2m_free(watcherP);
                    canceledCount++;
                }
                
                watcherP = nextWatcher;
            }
            
            // 如果观察对象没有观察者了，移除它
            if (observedP->watcherList == NULL)
            {
                COMPOSITE_TRACE("Removing empty observed object");
                prv_unlinkObserved(observedP);
                lwm2m_free(observedP->uriList);
                lwm2m_free(observedP);
            }
            
            observedP = nextObserved;
        }
    }
    
    COMPOSITE_TRACE("Cancelled %d observations", canceledCount);
    
    // 返回空的 SenML CBOR 数组作为响应
    if (response != NULL)
    {
        uint8_t *emptyArray = (uint8_t *)lwm2m_malloc(1);
        if (emptyArray != NULL)
        {
            emptyArray[0] = 0x80;  // CBOR empty array
            coap_set_header_content_type(response, LWM2M_CONTENT_SENML_CBOR);
            coap_set_payload(response, emptyArray, 1);
        }
    }
    
    COMPOSITE_TRACE("=== COMPOSITE CANCEL OBSERVE DONE === cancelled=%d", canceledCount);
    return result;
}

// ======================== 定期检查和通知机制 ========================

/**
 * 检查数据是否发生了变化
 */
static bool prv_hasDataChanged(lwm2m_data_t *oldData, int oldSize,
                               lwm2m_data_t *newData, int newSize)
{
    if (oldSize != newSize) return true;
    
    // 简化的变化检测：比较数据大小或内容
    // 更完善的实现应该比较每个资源的值
    for (int i = 0; i < oldSize; i++)
    {
        if (oldData[i].id != newData[i].id ||
            oldData[i].type != newData[i].type)
        {
            return true;
        }
        
        // 比较值（根据类型）
        switch (oldData[i].type)
        {
        case LWM2M_TYPE_INTEGER:
        {
            int64_t oldVal, newVal;
            if (lwm2m_data_decode_int(&oldData[i], &oldVal) &&
                lwm2m_data_decode_int(&newData[i], &newVal) &&
                oldVal != newVal)
            {
                return true;
            }
            break;
        }
        case LWM2M_TYPE_UNSIGNED_INTEGER:
        {
            uint64_t oldVal, newVal;
            if (lwm2m_data_decode_uint(&oldData[i], &oldVal) &&
                lwm2m_data_decode_uint(&newData[i], &newVal) &&
                oldVal != newVal)
            {
                return true;
            }
            break;
        }
        case LWM2M_TYPE_FLOAT:
        {
            double oldVal, newVal;
            if (lwm2m_data_decode_float(&oldData[i], &oldVal) &&
                lwm2m_data_decode_float(&newData[i], &newVal))
            {
                // 使用epsilon比较浮点数，避免直接使用==或!=
                double epsilon = 1e-9;
                double diff = (oldVal > newVal) ? (oldVal - newVal) : (newVal - oldVal);
                if (diff > epsilon)
                {
                    return true;
                }
            }
            break;
        }
        default:
            break;
        }
    }
    
    return false;
}

/**
 * 发送复合观察通知
 */
static void prv_sendCompositeNotification(lwm2m_context_t *contextP,
                                         prv_composite_watcher_t *watcherP,
                                         uint8_t *payload, size_t payloadLen)
{
    if (watcherP == NULL || watcherP->server == NULL)
    {
        COMPOSITE_ERROR("Invalid watcher or server");
        return;
    }
    
    COMPOSITE_TRACE("Sending notification: token_len=%d, counter=%u, payload_len=%zu",
                   watcherP->token_len, watcherP->counter, payloadLen);
    
    coap_packet_t message[1];
    memset(message, 0, sizeof(coap_packet_t));
    
    // 构造通知消息
    message->code = COAP_205_CONTENT;
    message->type = COAP_TYPE_NON;  // 非确认消息
    message->mid = contextP->nextMID++;
    
    // 设置 Token
    message->token_len = watcherP->token_len;
    if (watcherP->token_len > 0)
    {
        memcpy(message->token, watcherP->token, watcherP->token_len);
    }
    
    // 设置内容类型和 Observe 选项
    coap_set_header_content_type(message, LWM2M_CONTENT_SENML_CBOR);
    coap_set_header_observe(message, watcherP->counter++);
    coap_set_payload(message, payload, payloadLen);
    
    // 发送通知
    uint8_t sendResult = message_send(contextP, message, watcherP->server->sessionH);
    
    if (sendResult == COAP_NO_ERROR)
    {
        watcherP->lastTime = lwm2m_gettime();
        watcherP->lastMid = message->mid;
        COMPOSITE_TRACE("Notification sent successfully, counter incremented to %u", watcherP->counter);
    }
    else
    {
        COMPOSITE_ERROR("Failed to send notification: result=%d", sendResult);
    }
}

/**
 * 定期检查复合观察并发送通知（应该由主事件循环定期调用）
 */
void composite_step(lwm2m_context_t *contextP, time_t currentTime, time_t *timeoutP)
{
    if (contextP == NULL || g_composite_observedList == NULL)
    {
        return;
    }
    
    COMPOSITE_TRACE("=== COMPOSITE STEP START === currentTime=%lld", (long long)currentTime);
    
    prv_composite_observed_t *observedP = g_composite_observedList;
    
    while (observedP != NULL)
    {
        prv_composite_watcher_t *watcherP = observedP->watcherList;
        
        while (watcherP != NULL)
        {
            if (watcherP->active)
            {
                bool shouldNotify = false;
                
                // 读取当前数据
                int currentSize = 0;
                lwm2m_data_t *currentData = NULL;
                
                if (object_readCompositeData(contextP, observedP->uriList, observedP->uriCount, 
                                            &currentSize, &currentData) == COAP_205_CONTENT)
                {
                    // 检查数据是否变化
                    if (watcherP->lastDataArray != NULL && 
                        prv_hasDataChanged(watcherP->lastDataArray, watcherP->lastDataSize,
                                         currentData, currentSize))
                    {
                        COMPOSITE_TRACE("Data change detected for %d URIs", observedP->uriCount);
                        shouldNotify = true;
                        
                        // 更新缓存的数据
                        lwm2m_data_free(watcherP->lastDataSize, watcherP->lastDataArray);
                        watcherP->lastDataArray = currentData;
                        watcherP->lastDataSize = currentSize;
                    }
                    else if (watcherP->lastDataArray == NULL)
                    {
                        // 第一次初始化
                        watcherP->lastDataArray = currentData;
                        watcherP->lastDataSize = currentSize;
                    }
                    else
                    {
                        // 数据没有变化，释放临时数据
                        lwm2m_data_free(currentSize, currentData);
                    }
                    
                    // 检查最大周期
                    if (!shouldNotify && watcherP->parameters != NULL &&
                        (watcherP->parameters->toSet & LWM2M_ATTR_FLAG_MAX_PERIOD) != 0)
                    {
                        time_t elapsed = currentTime - watcherP->lastTime;
                        if (elapsed >= watcherP->parameters->maxPeriod)
                        {
                            COMPOSITE_TRACE("Maximum period reached for %d URIs", observedP->uriCount);
                            shouldNotify = true;
                        }
                    }
                    
                    // 如果应该通知，编码并发送
                    if (shouldNotify)
                    {
                        uint8_t *payload = NULL;
                        size_t payloadLen = 0;
                        
                        int encodeResult = prv_encode_multi_resource(observedP->uriList, 
                                                                     observedP->uriCount,
                                                                     contextP, &payload, &payloadLen);
                        
                        if (encodeResult == COAP_205_CONTENT && payload != NULL)
                        {
                            prv_sendCompositeNotification(contextP, watcherP, payload, payloadLen);
                            lwm2m_free(payload);
                        }
                        else
                        {
                            COMPOSITE_ERROR("Failed to encode data for notification");
                        }
                    }
                }
            }
            
            watcherP = watcherP->next;
        }
        
        observedP = observedP->next;
    }
    
    COMPOSITE_TRACE("=== COMPOSITE STEP DONE ===");
}

/**
 * 清理所有复合观察（在关闭时调用）
 */
void composite_clear(lwm2m_context_t *contextP)
{
    (void)contextP;  // 未使用
    
    COMPOSITE_TRACE("=== COMPOSITE CLEAR START ===");
    
    prv_composite_observed_t *observedP = g_composite_observedList;
    
    while (observedP != NULL)
    {
        prv_composite_observed_t *nextObserved = observedP->next;
        prv_composite_watcher_t *watcherP = observedP->watcherList;
        
        while (watcherP != NULL)
        {
            prv_composite_watcher_t *nextWatcher = watcherP->next;
            
            if (watcherP->lastDataArray != NULL)
            {
                lwm2m_data_free(watcherP->lastDataSize, watcherP->lastDataArray);
            }
            
            if (watcherP->parameters != NULL)
            {
                lwm2m_free(watcherP->parameters);
            }
            
            lwm2m_free(watcherP);
            watcherP = nextWatcher;
        }
        
        lwm2m_free(observedP->uriList);
        lwm2m_free(observedP);
        observedP = nextObserved;
    }
    
    g_composite_observedList = NULL;
    
    COMPOSITE_TRACE("=== COMPOSITE CLEAR DONE ===");
}

/**
 * 获取当前活跃的复合观察数
 */
int composite_get_observe_count(void)
{
    int count = 0;
    prv_composite_observed_t *observedP = g_composite_observedList;
    
    while (observedP != NULL)
    {
        prv_composite_watcher_t *watcherP = observedP->watcherList;
        while (watcherP != NULL)
        {
            if (watcherP->active)
            {
                count++;
            }
            watcherP = watcherP->next;
        }
        observedP = observedP->next;
    }
    
    return count;
}

