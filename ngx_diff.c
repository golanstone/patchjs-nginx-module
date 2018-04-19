#include <ngx_md5.h>
#include "hash_table.h"
#include "ngx_diff.h"

#define CHUNK_SIZE 20
#define MAX_CONTENT_SIZE (2*1024*1024)

ngx_pool_t *g_pool = NULL;

static void checksum(ngx_http_request_t *r, HashTable *ht, u_char* file_cnt, ngx_uint_t len);
static ngx_uint_t roll(ngx_http_request_t *r, HashTable *ht, ngx_array_t *diff_array, u_char* file_cnt, ngx_uint_t len);
static ngx_int_t find_match_order_id(ngx_http_request_t *r, HashTable *ht, u_char *md5_result, ngx_uint_t last_order_id);
static u_char* make_md5(const u_char* cnt, ngx_uint_t len, u_char *result);

typedef struct diff_data{
	u_char match;
	ngx_int_t order_id;
	u_char *start;
	ngx_uint_t len;
} DiffData;

typedef struct result {
	struct HashTable *ht_dst;
	struct HashTable *ht_src;

	ngx_pool_t *pool;
	ngx_array_t *diff_array;
} Result;

ngx_str_t * calc_diff_data(ngx_http_request_t *r, u_char* src_file_cnt, ngx_uint_t src_len, u_char* dst_file_cnt, ngx_uint_t dst_len)
{
	ngx_str_t *res = (ngx_str_t *)ngx_palloc(r->pool, sizeof(ngx_str_t));
	u_char prefix[32] = {0};
	ngx_str_t tail = ngx_string("]}");
	ngx_uint_t move_len = 0;

	Result *result = ngx_palloc(r->pool, sizeof(Result));
	ngx_pool_t *pool = r->pool;
	result->pool = pool;

	result->ht_dst = hash_table_new(pool);
	result->ht_src = hash_table_new(pool);

	ngx_memzero(prefix, sizeof(prefix));
	u_char src_md5[16], dst_md5[16];
	make_md5(src_file_cnt, src_len, src_md5);
	make_md5(dst_file_cnt, dst_len, dst_md5);
	if (ngx_strcmp(src_md5, dst_md5) == 0) {
		ngx_sprintf(prefix, "{\"m\":false,\"l\":%d,\"c\":[]}", CHUNK_SIZE);
		res->len = ngx_strlen(prefix);
		res->data = (u_char*)ngx_palloc(pool, sizeof(u_char) * res->len);
		ngx_memcpy(res->data, prefix, res->len);

		return res;
	}

	result->diff_array = (ngx_array_t*)ngx_array_create(pool, 16, sizeof(DiffData));
	checksum(r, result->ht_dst, dst_file_cnt, dst_len); // calc remote file context hash table
	roll(r, result->ht_dst, result->diff_array, src_file_cnt, src_len); // calc remote file context hash table

	// 准备res内存
	res->len = src_len + 32;
	res->data = (u_char*)ngx_palloc(pool, res->len);
	ngx_memzero(res->data, sizeof(u_char)*res->len);
	u_char *p_content = res->data;

	// 组头
	ngx_sprintf(prefix, "{\"m\":true,\"l\":%d,\"c\":[", CHUNK_SIZE);
	move_len = ngx_strlen(prefix);
	ngx_memcpy(p_content, prefix, move_len);
	p_content += move_len;

	DiffData *last_item = NULL;
	ngx_uint_t match_count = 0;
	DiffData *p = (DiffData *)result->diff_array->elts;
	for (int i = 0, size = result->diff_array->nelts; i < size; i++) {
		DiffData *item = p + i;
		u_char temp[16] = {0};
		ngx_uint_t _len = 0;

		if (item->match) {
			if (last_item == NULL || !last_item->match) {
				ngx_snprintf(temp, 16, "[%d,", item->order_id);
				_len = ngx_strlen(temp);
				ngx_memcpy(p_content, temp, _len);
				p_content += _len;
				match_count = 1;
			} else if (last_item->match && (last_item->order_id+1) == item->order_id) {
				match_count++;
			} else if (last_item->match && (last_item->order_id+1) != item->order_id) {
				ngx_snprintf(temp, 16, "%d", match_count);
				_len = ngx_strlen(temp);
				ngx_memcpy(p_content, temp, _len);
				p_content += _len;
				match_count = 1;
			}

			if (i == size - 1) {
				ngx_snprintf(temp, 16, "%d]", match_count);
				_len = ngx_strlen(temp);
				ngx_memcpy(p_content, temp, _len);
				p_content += _len;
			}
		} else {
			if (match_count > 0) {
				ngx_snprintf(temp, 16, "%d]", match_count);
				_len = ngx_strlen(temp);
				ngx_memcpy(p_content, temp, ngx_strlen(temp));
				p_content += _len;
				match_count = 0;
			}

			*p_content++ = ',';
			*p_content++ = '"';
			ngx_memcpy(p_content, item->start, item->len);
			p_content += item->len;
			*p_content++ = '"';
		}
		last_item = item;
	}

	// 组尾
	ngx_memcpy(p_content, tail.data, tail.len);
	p_content += tail.len;

	return res;
}

// // 计算md5 不能改变cnt的内容const
static u_char* make_md5(const u_char* cnt, ngx_uint_t len, u_char *result) 
{
	// u_char result[16];
	ngx_md5_t ctx;
	ngx_md5_init(&ctx);
	ngx_md5_update(&ctx, cnt, len);
	ngx_md5_final(result, &ctx);
	return result;
}

static void checksum(ngx_http_request_t *r, struct HashTable *ht, u_char* file_cnt, ngx_uint_t len)
{
	ngx_pool_t *pool = r->pool;
	u_char *p = file_cnt;
	ngx_uint_t order_id = 0;
	for (ngx_uint_t i=0; i<len; ) {
		ngx_uint_t get_size = CHUNK_SIZE;
		ngx_array_t *order_ids = NULL;
		ngx_uint_t *p_order_id = NULL;

		if (len - 1 - i < CHUNK_SIZE) {
			get_size = CHUNK_SIZE - (len - 1 - i);
		}	

		u_char md5_value[16] = {0};
		make_md5(p, get_size, md5_value);
		order_ids = (ngx_array_t *)hash_table_get(ht, (char *)md5_value);
		if (order_ids == NULL) {
			order_ids = ngx_array_create(pool, 4, sizeof(ngx_uint_t));
			p_order_id = ngx_array_push(order_ids); // 添加一个元素
			*p_order_id = order_id++;
			hash_table_put(ht, (char *)md5_value, order_ids);
		} else {
			p_order_id = ngx_array_push(order_ids); // 添加一个元素
			*p_order_id = order_id++;
		}

		p += get_size;
		i += get_size;
	}
}

static ngx_uint_t roll(ngx_http_request_t *r, HashTable *ht, ngx_array_t *diff_array, u_char* file_cnt, ngx_uint_t len)
{
	ngx_uint_t new_content_size = 0;
	u_char *p = file_cnt; // 本地文件
	// ngx_uint_t order_id = 1; // 初始chunk id

	u_char *unmatch_start = file_cnt; // 指向开始不匹配的的地址
	ngx_uint_t unmatch_len = 0; // 不匹配的长度

	ngx_int_t last_order_id = -1; // 

	for (ngx_uint_t i=0; i<len; ) {
		// ngx_int_t m_order_id = -1;

		DiffData *match_diff = NULL;
		DiffData *unmatch_diff = NULL;
		// 或许chunk_size 剩余长度不足一个chunk大小 则有多少取多少
		ngx_uint_t get_size = CHUNK_SIZE;
		if (len - 1 - i < CHUNK_SIZE) {
			get_size = CHUNK_SIZE - (len -1 - i);
		}

        // 在目标文件的hash table中找此chunk的md5, 如果找到了p_order_id不为NULL，反之亦然
		u_char md5_result[16] = {0};
		make_md5(p, get_size, md5_result);
		ngx_int_t match_order_id = find_match_order_id(r, ht, md5_result, last_order_id);
		if (match_order_id == -1) { // 未找到
			i++;
			unmatch_len++;
			p++;
		} else { // 已找到
			// ngx_uint_t match_order_id = int(m_order_id); // 目标文件的chunk ID    类型转换 void * 转化为 int

            //  unmatch_size大于0的话，说明匹配chunk成功之前有不能匹配的数据, 所以先把不匹配的数据组成一个不定长度的chunk，再对匹配的chunk做记录
			if (unmatch_len > 0) {
				unmatch_diff = ngx_array_push(diff_array); // 添加一个元素
				unmatch_diff->match = 0;
				unmatch_diff->order_id = -1;
				unmatch_diff->start = unmatch_start;
				unmatch_diff->len = unmatch_len;
				
				new_content_size += unmatch_len + 3;
			}

			match_diff = ngx_array_push(diff_array); // 添加一个元素
			match_diff->match = 1;
			match_diff->order_id = match_order_id;
			match_diff->start = NULL;
			match_diff->len = 0;

			new_content_size += 10;

			p += get_size;
			i += get_size;

			unmatch_start = p;
			unmatch_len = 0;
			last_order_id = match_order_id;
		}

		if (i == len - 1) {
			if (unmatch_len > 0) {
				// ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "patchjs  rool ----2-0");
				unmatch_diff = ngx_array_push(diff_array); // 添加一个元素
				unmatch_diff->match = 0;
				unmatch_diff->order_id = -1;
				unmatch_diff->start = unmatch_start;
				unmatch_diff->len = unmatch_len;

				new_content_size += unmatch_len + 3;
			}
		}
	}

	return 0;
}

static ngx_int_t find_match_order_id(ngx_http_request_t *r, HashTable *ht, u_char *md5_result, ngx_uint_t last_order_id)
{
	ngx_array_t *value = (ngx_array_t*)hash_table_get(ht, (char *)md5_result);
	if (value == NULL) {
		return -1;
	}

	ngx_uint_t *order_ids = value->elts;
	if (value->nelts == 1) {
		return order_ids[0];
	} else {
		ngx_uint_t last_id = order_ids[0];
		ngx_uint_t result_id = 0;
		for (ngx_uint_t i=0; i<value->nelts; i++) {
			ngx_uint_t id = order_ids[0];
			if (id >= last_order_id && last_id <= last_order_id) {
				return (last_order_id - last_id) >= (id - last_order_id) ? id : last_id;
			} else if (id >= last_order_id && last_id <= last_order_id) {
				return last_id;
			} else if (id <= last_order_id && last_id <= last_order_id) {
				return id;
			} else {
				result_id = id;
			}
		}
		return result_id;
	}

	return order_ids[0];
}
