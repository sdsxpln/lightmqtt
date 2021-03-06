#include "check_lightmqtt.h"

#define BYTES_W_PLACEHOLDER ((size_t) -12345)
#define BUF_PLACEHOLDER 0xcc
#define ENTRY_COUNT 16

#define PREPARE \
    int res; \
    int data = 0; \
    size_t bytes_w = BYTES_W_PLACEHOLDER; \
    unsigned char buf[256]; \
    lmqtt_tx_buffer_t state; \
    lmqtt_store_t store; \
    lmqtt_store_value_t value; \
    lmqtt_store_entry_t entries[ENTRY_COUNT]; \
    memset(encoders, 0, sizeof(encoders)); \
    memset(&state, 0, sizeof(state)); \
    memset(&store, 0, sizeof(store)); \
    memset(&value, 0, sizeof(value)); \
    memset(entries, 0, sizeof(entries)); \
    state.store = &store; \
    store.get_time = &test_time_get; \
    store.entries = entries; \
    store.capacity = ENTRY_COUNT; \
    value.value = &data; \
    test_finder_func = &test_finder; \
    memset(buf, BUF_PLACEHOLDER, sizeof(buf))

/*
 * To easily test the tx buffer building we use very simple encoding methods
 * named `encode_test_<begin>_<end>`, meaning it fills the given buffer with the
 * the value pointed to by `data`, adding the values from <begin> to <end>. For
 * example, given data=100 and offset=0 the method `encode_test_0_9` fills the
 * buffer with values (100, 101, 102, ... 108, 109). Given data=10 and offset=8
 * the buffer is filled with (18, 19).
 *
 * `block_at` sets the last "non-blocking" read. If `begin + offset` is past
 * `block_at` the method returns LMQTT_ENCODE_WOULD_BLOCK.
 */

static lmqtt_encoder_t encoders[10];

static lmqtt_encode_result_t encode_test_range(lmqtt_store_value_t *value,
    size_t offset, unsigned char *buf, size_t buf_len, size_t *bytes_written,
    size_t begin, size_t end, size_t block_at)
{
    size_t i;
    size_t pos = 0;
    int *data = value->value;

    assert(buf_len >= 0);

    *bytes_written = 0;

    if (begin + offset >= block_at && block_at <= end)
        return LMQTT_ENCODE_WOULD_BLOCK;

    for (i = begin + offset; i <= end; i++) {
        if (pos >= buf_len || i >= block_at)
            return LMQTT_ENCODE_CONTINUE;
        buf[pos++] = *data + i;
        *bytes_written += 1;
    }
    return LMQTT_ENCODE_FINISHED;
}

static lmqtt_encoder_finder_t test_finder_func;

static lmqtt_encoder_t test_finder(lmqtt_tx_buffer_t *tx_buffer,
    lmqtt_store_value_t *value)
{
    return encoders[tx_buffer->internal.pos];
}

lmqtt_encoder_finder_t tx_buffer_finder_by_kind_mock(
    lmqtt_kind_t kind)
{
    return test_finder_func;
}

static lmqtt_encode_result_t encode_test_0_9(lmqtt_store_value_t *value,
    lmqtt_encode_buffer_t *encode_buffer, size_t offset, unsigned char *buf,
    size_t buf_len, size_t *bytes_written)
{
    return encode_test_range(value, offset, buf, buf_len, bytes_written, 0, 9, 10);
}

static lmqtt_encode_result_t encode_test_50_54(lmqtt_store_value_t *value,
    lmqtt_encode_buffer_t *encode_buffer, size_t offset, unsigned char *buf,
    size_t buf_len, size_t *bytes_written)
{
    return encode_test_range(value, offset, buf, buf_len, bytes_written, 50, 54, 55);
}

static lmqtt_encode_result_t encode_test_10_19_blocking(
    lmqtt_store_value_t *value, lmqtt_encode_buffer_t *encode_buffer,
    size_t offset, unsigned char *buf, size_t buf_len, size_t *bytes_written)
{
    return encode_test_range(value, offset, buf, buf_len, bytes_written, 10, 19, 15);
}

static lmqtt_encode_result_t encode_test_fail(lmqtt_store_value_t *value,
    lmqtt_encode_buffer_t *encode_buffer, size_t offset, unsigned char *buf,
    size_t buf_len, size_t *bytes_written)
{
    encode_buffer->error = LMQTT_ERROR_ENCODE_STRING;
    encode_buffer->os_error = 0xffff;
    return LMQTT_ENCODE_ERROR;
}

START_TEST(should_encode_tx_buffer_with_one_encoding_function)
{
    PREPARE;

    encoders[0] = encode_test_0_9;
    lmqtt_store_append(&store, 0, &value);

    res = lmqtt_tx_buffer_encode(&state, buf, sizeof(buf), &bytes_w);

    ck_assert_int_eq(LMQTT_IO_SUCCESS, res);
    ck_assert_int_eq(10, bytes_w);

    ck_assert_uint_eq(0, buf[0]);
    ck_assert_uint_eq(9, buf[9]);
    ck_assert_uint_eq(BUF_PLACEHOLDER, buf[10]);
}
END_TEST

START_TEST(should_encode_tx_buffer_with_two_encoding_functions)
{
    PREPARE;

    encoders[0] = (lmqtt_encoder_t) encode_test_0_9;
    encoders[1] = (lmqtt_encoder_t) encode_test_50_54;
    lmqtt_store_append(&store, 0, &value);

    res = lmqtt_tx_buffer_encode(&state, buf, sizeof(buf), &bytes_w);

    ck_assert_int_eq(LMQTT_IO_SUCCESS, res);
    ck_assert_int_eq(15, bytes_w);

    ck_assert_uint_eq(0, buf[0]);
    ck_assert_uint_eq(9, buf[9]);
    ck_assert_uint_eq(50, buf[10]);
    ck_assert_uint_eq(54, buf[14]);
    ck_assert_uint_eq(BUF_PLACEHOLDER, buf[15]);
}
END_TEST

START_TEST(should_stop_encoder_after_buffer_fills_up)
{
    PREPARE;

    encoders[0] = (lmqtt_encoder_t) encode_test_0_9;
    encoders[1] = (lmqtt_encoder_t) encode_test_50_54;
    lmqtt_store_append(&store, 0, &value);

    res = lmqtt_tx_buffer_encode(&state, buf, 5, &bytes_w);

    ck_assert_int_eq(LMQTT_IO_SUCCESS, res);
    ck_assert_int_eq(5, bytes_w);

    ck_assert_uint_eq(0, buf[0]);
    ck_assert_uint_eq(4, buf[4]);
    ck_assert_uint_eq(BUF_PLACEHOLDER, buf[5]);
}
END_TEST

START_TEST(should_return_actual_bytes_written_after_error)
{
    PREPARE;

    encoders[0] = (lmqtt_encoder_t) encode_test_0_9;
    encoders[1] = (lmqtt_encoder_t) encode_test_50_54;
    encoders[2] = (lmqtt_encoder_t) encode_test_fail;
    lmqtt_store_append(&store, 0, &value);

    res = lmqtt_tx_buffer_encode(&state, buf, sizeof(buf), &bytes_w);

    ck_assert_int_eq(LMQTT_IO_ERROR, res);
    ck_assert_int_eq(15, bytes_w);

    ck_assert_uint_eq(0, buf[0]);
    ck_assert_uint_eq(54, buf[14]);
    ck_assert_uint_eq(BUF_PLACEHOLDER, buf[15]);
}
END_TEST

START_TEST(should_not_encode_tx_buffer_after_error)
{
    PREPARE;

    encoders[0] = (lmqtt_encoder_t) encode_test_fail;
    lmqtt_store_append(&store, 0, &value);

    res = lmqtt_tx_buffer_encode(&state, buf, sizeof(buf), &bytes_w);
    ck_assert_int_eq(LMQTT_IO_ERROR, res);

    encoders[0] = (lmqtt_encoder_t) encode_test_0_9;
    bytes_w = 0xcccc;

    res = lmqtt_tx_buffer_encode(&state, buf, sizeof(buf), &bytes_w);
    ck_assert_int_eq(LMQTT_IO_ERROR, res);
    ck_assert_int_eq(0, bytes_w);
}
END_TEST

START_TEST(should_encode_tx_buffer_again_after_reset)
{
    PREPARE;

    encoders[0] = (lmqtt_encoder_t) encode_test_fail;
    lmqtt_store_append(&store, 0, &value);

    res = lmqtt_tx_buffer_encode(&state, buf, sizeof(buf), &bytes_w);

    encoders[0] = (lmqtt_encoder_t) encode_test_0_9;
    lmqtt_tx_buffer_reset(&state);

    res = lmqtt_tx_buffer_encode(&state, buf, sizeof(buf), &bytes_w);
    ck_assert_int_eq(LMQTT_IO_SUCCESS, res);
}
END_TEST

START_TEST(should_read_error_after_encode_error)
{
    int os_error = 0xcccc;
    lmqtt_error_t error;

    PREPARE;

    encoders[0] = (lmqtt_encoder_t) encode_test_fail;
    lmqtt_store_append(&store, 0, &value);

    res = lmqtt_tx_buffer_encode(&state, buf, sizeof(buf), &bytes_w);
    ck_assert_int_eq(LMQTT_IO_ERROR, res);

    error = lmqtt_tx_buffer_get_error(&state, &os_error);
    ck_assert_int_eq(LMQTT_ERROR_ENCODE_STRING, error);
    ck_assert_int_eq(0xffff, os_error);
}
END_TEST

START_TEST(should_continue_buffer_where_previous_call_stopped)
{
    PREPARE;

    encoders[0] = (lmqtt_encoder_t) encode_test_0_9;
    encoders[1] = (lmqtt_encoder_t) encode_test_50_54;
    lmqtt_store_append(&store, 0, &value);

    res = lmqtt_tx_buffer_encode(&state, buf, 6, &bytes_w);

    ck_assert_int_eq(LMQTT_IO_SUCCESS, res);
    ck_assert_int_eq(6, bytes_w);

    ck_assert_uint_eq(0, buf[0]);
    ck_assert_uint_eq(5, buf[5]);
    ck_assert_uint_eq(BUF_PLACEHOLDER, buf[6]);

    res = lmqtt_tx_buffer_encode(&state, buf, 6, &bytes_w);

    ck_assert_int_eq(LMQTT_IO_SUCCESS, res);
    ck_assert_int_eq(6, bytes_w);

    ck_assert_uint_eq(6, buf[0]);
    ck_assert_uint_eq(51, buf[5]);
    ck_assert_uint_eq(BUF_PLACEHOLDER, buf[6]);

    res = lmqtt_tx_buffer_encode(&state, buf, 6, &bytes_w);

    ck_assert_int_eq(LMQTT_IO_SUCCESS, res);
    ck_assert_int_eq(3, bytes_w);

    ck_assert_uint_eq(52, buf[0]);
    ck_assert_uint_eq(54, buf[2]);
}
END_TEST

START_TEST(should_continue_buffer_twice_with_the_same_encoder_entry)
{
    PREPARE;

    encoders[0] = (lmqtt_encoder_t) encode_test_0_9;
    lmqtt_store_append(&store, 0, &value);

    res = lmqtt_tx_buffer_encode(&state, buf, 2, &bytes_w);

    res = lmqtt_tx_buffer_encode(&state, buf, 2, &bytes_w);

    ck_assert_int_eq(LMQTT_IO_SUCCESS, res);
    ck_assert_uint_eq(2, buf[0]);
    ck_assert_uint_eq(3, buf[1]);

    res = lmqtt_tx_buffer_encode(&state, buf, 2, &bytes_w);

    ck_assert_int_eq(LMQTT_IO_SUCCESS, res);
    ck_assert_uint_eq(4, buf[0]);
    ck_assert_uint_eq(5, buf[1]);
}
END_TEST

START_TEST(should_encode_blocking_buffer)
{
    PREPARE;

    encoders[0] = (lmqtt_encoder_t) encode_test_10_19_blocking;
    lmqtt_store_append(&store, 0, &value);

    res = lmqtt_tx_buffer_encode(&state, buf, sizeof(buf), &bytes_w);

    ck_assert_int_eq(LMQTT_IO_SUCCESS, res);
    ck_assert_int_eq(5, bytes_w);

    ck_assert_uint_eq(10, buf[0]);
    ck_assert_uint_eq(14, buf[4]);
    ck_assert_uint_eq(BUF_PLACEHOLDER, buf[5]);

    res = lmqtt_tx_buffer_encode(&state, buf, sizeof(buf), &bytes_w);

    ck_assert_int_eq(LMQTT_IO_WOULD_BLOCK, res);
    ck_assert_int_eq(0, bytes_w);
}
END_TEST

START_TEST(should_not_encode_null_encoder)
{
    PREPARE;

    res = lmqtt_tx_buffer_encode(&state, buf, sizeof(buf), &bytes_w);

    ck_assert_int_eq(LMQTT_IO_WOULD_BLOCK, res);
    ck_assert_int_eq(0, bytes_w);
}
END_TEST

START_TEST(should_encode_two_packets_at_once)
{
    int data2 = 100;
    PREPARE;

    encoders[0] = (lmqtt_encoder_t) encode_test_0_9;
    lmqtt_store_append(&store, 0, &value);
    value.packet_id = 1;
    value.value = &data2;
    lmqtt_store_append(&store, 0, &value);

    res = lmqtt_tx_buffer_encode(&state, buf, sizeof(buf), &bytes_w);

    ck_assert_int_eq(LMQTT_IO_SUCCESS, res);
    ck_assert_int_eq(20, bytes_w);

    ck_assert_uint_eq(0, buf[0]);
    ck_assert_uint_eq(9, buf[9]);
    ck_assert_uint_eq(100, buf[10]);
    ck_assert_uint_eq(109, buf[19]);
    ck_assert_uint_eq(BUF_PLACEHOLDER, buf[20]);
}
END_TEST

START_TEST(should_not_track_disconnect_packet)
{
    int kind;
    lmqtt_store_value_t value_out;
    PREPARE;

    encoders[0] = (lmqtt_encoder_t) encode_test_0_9;
    lmqtt_store_append(&store, LMQTT_KIND_DISCONNECT, &value);

    res = lmqtt_tx_buffer_encode(&state, buf, sizeof(buf), &bytes_w);
    ck_assert_int_eq(LMQTT_IO_SUCCESS, res);

    res = lmqtt_store_shift(&store, &kind, &value_out);
    ck_assert_int_eq(0, res);
}
END_TEST

START_TEST(should_close_tx_buffer_on_disconnect)
{
    PREPARE;

    encoders[0] = (lmqtt_encoder_t) encode_test_0_9;
    lmqtt_store_append(&store, LMQTT_KIND_DISCONNECT, &value);

    res = lmqtt_tx_buffer_encode(&state, buf, sizeof(buf), &bytes_w);
    ck_assert_int_eq(LMQTT_IO_SUCCESS, res);
    ck_assert_int_eq(10, bytes_w);

    res = lmqtt_tx_buffer_encode(&state, buf, sizeof(buf), &bytes_w);
    ck_assert_int_eq(LMQTT_IO_SUCCESS, res);
    ck_assert_int_eq(0, bytes_w);
}
END_TEST

START_TEST(should_not_process_packets_after_disconnect)
{
    int data2 = 100;
    PREPARE;

    encoders[0] = (lmqtt_encoder_t) encode_test_0_9;
    lmqtt_store_append(&store, LMQTT_KIND_DISCONNECT, &value);
    value.packet_id = 1;
    value.value = &data2;
    lmqtt_store_append(&store, LMQTT_KIND_PUBLISH_1, &value);

    res = lmqtt_tx_buffer_encode(&state, buf, sizeof(buf), &bytes_w);
    ck_assert_int_eq(LMQTT_IO_SUCCESS, res);
    ck_assert_int_eq(10, bytes_w);
}
END_TEST

START_TEST(should_track_connect_packet)
{
    lmqtt_store_value_t value_out;
    PREPARE;

    encoders[0] = (lmqtt_encoder_t) encode_test_0_9;
    lmqtt_store_append(&store, LMQTT_KIND_CONNECT, &value);

    res = lmqtt_tx_buffer_encode(&state, buf, sizeof(buf), &bytes_w);
    ck_assert_int_eq(LMQTT_IO_SUCCESS, res);

    res = lmqtt_store_pop_marked_by(&store, LMQTT_KIND_CONNECT, 0, &value_out);
    ck_assert_int_eq(1, res);
}
END_TEST

START_TEST(should_clear_encoder_state_after_reset)
{
    PREPARE;

    encoders[0] = (lmqtt_encoder_t) encode_test_0_9;
    lmqtt_store_append(&store, 0, &value);

    res = lmqtt_tx_buffer_encode(&state, buf, 4, &bytes_w);
    ck_assert_int_eq(LMQTT_IO_SUCCESS, res);

    lmqtt_tx_buffer_reset(&state);

    res = lmqtt_tx_buffer_encode(&state, buf, sizeof(buf), &bytes_w);
    ck_assert_int_eq(LMQTT_IO_SUCCESS, res);
    ck_assert_int_eq(10, bytes_w);

    ck_assert_uint_eq(0, buf[0]);
    ck_assert_uint_eq(4, buf[4]);
    ck_assert_uint_eq(9, buf[9]);
    ck_assert_uint_eq(BUF_PLACEHOLDER, buf[10]);
}
END_TEST

START_TCASE("Tx buffer encode")
{
    tx_buffer_finder_by_kind = &tx_buffer_finder_by_kind_mock;

    ADD_TEST(should_encode_tx_buffer_with_one_encoding_function);
    ADD_TEST(should_encode_tx_buffer_with_two_encoding_functions);
    ADD_TEST(should_stop_encoder_after_buffer_fills_up);
    ADD_TEST(should_return_actual_bytes_written_after_error);
    ADD_TEST(should_not_encode_tx_buffer_after_error);
    ADD_TEST(should_encode_tx_buffer_again_after_reset);
    ADD_TEST(should_read_error_after_encode_error);
    ADD_TEST(should_continue_buffer_where_previous_call_stopped);
    ADD_TEST(should_continue_buffer_twice_with_the_same_encoder_entry);
    ADD_TEST(should_encode_blocking_buffer);
    ADD_TEST(should_not_encode_null_encoder);
    ADD_TEST(should_encode_two_packets_at_once);
    ADD_TEST(should_not_track_disconnect_packet);
    ADD_TEST(should_close_tx_buffer_on_disconnect);
    ADD_TEST(should_not_process_packets_after_disconnect);
    ADD_TEST(should_track_connect_packet);
    ADD_TEST(should_clear_encoder_state_after_reset);
}
END_TCASE
