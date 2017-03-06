#include "check_lightmqtt.h"
#include <assert.h>

#include "../src/lmqtt_io.c"

enum {
    TEST_CONNECT = 1,
    TEST_PINGREQ,
    TEST_CONNACK_SUCCESS,
    TEST_CONNACK_FAILURE,
    TEST_PINGRESP
} test_type_t;

static struct {
    long secs;
    long nsecs;
} test_time;

static lmqtt_io_result_t test_time_get(long *secs, long *nsecs)
{
    *secs = test_time.secs;
    *nsecs = test_time.nsecs;
    return LMQTT_IO_SUCCESS;
}

static void test_time_set(long secs, long nsecs)
{
    test_time.secs = secs;
    test_time.nsecs = nsecs;
}

typedef struct {
    test_buffer_t read_buf;
    test_buffer_t write_buf;
    int test_pos_read;
    int test_pos_write;
} test_socket_t;

static test_socket_t test_socket;
static u8 test_recipe;

void lmqtt_tx_buffer_connect(lmqtt_tx_buffer_t *state,
    lmqtt_connect_t *connect)
{
    memset(state, 0, sizeof(*state));
    state->recipe_data = &test_recipe;
    test_recipe = TEST_CONNECT;
}

void lmqtt_tx_buffer_pingreq(lmqtt_tx_buffer_t *state)
{
    memset(state, 0, sizeof(*state));
    state->recipe_data = &test_recipe;
    test_recipe = TEST_PINGREQ;
}

lmqtt_io_result_t lmqtt_tx_buffer_encode(lmqtt_tx_buffer_t *state, u8 *buf,
    int buf_len, int *bytes_written)
{
    u8 *data = (u8 *) state->recipe_data;

    *bytes_written = 0;

    if (*data != 0) {
        assert(buf_len > 0);
        buf[0] = *data;
        *data = 0;
        *bytes_written = 1;
    }
    return LMQTT_IO_SUCCESS;
}

lmqtt_io_result_t lmqtt_rx_buffer_decode(lmqtt_rx_buffer_t *state, u8 *buf,
    int buf_len, int *bytes_read)
{
    int i;
    lmqtt_connack_t connack;

    for (i = 0; i < buf_len; i++) {
        switch (buf[i]) {
        case TEST_CONNACK_SUCCESS:
            memset(&connack, 0, sizeof(connack));
            state->callbacks->on_connack(state->callbacks_data, &connack);
            break;
        case TEST_CONNACK_FAILURE:
            memset(&connack, 0, sizeof(connack));
            connack.return_code = 1;
            state->callbacks->on_connack(state->callbacks_data, &connack);
            break;
        case TEST_PINGRESP:
            state->callbacks->on_pingresp(state->callbacks_data);
            break;
        }
    }

    *bytes_read = buf_len;
    return LMQTT_IO_SUCCESS;
}

static lmqtt_io_result_t test_socket_read(void *data, u8 *buf, int buf_len,
    int *bytes_read)
{
    test_socket_t *sock = (test_socket_t *) data;

    return test_buffer_move(&sock->read_buf,
        buf, &sock->read_buf.buf[sock->read_buf.pos],
        buf_len, bytes_read);
}

static lmqtt_io_result_t test_socket_write(void *data, u8 *buf, int buf_len,
    int *bytes_written)
{
    test_socket_t *sock = (test_socket_t *) data;

    return test_buffer_move(&sock->write_buf,
        &sock->write_buf.buf[sock->write_buf.pos], buf,
        buf_len, bytes_written);
}

static void test_socket_init_with_client(lmqtt_client_t *client)
{
    memset(&test_socket, 0, sizeof(test_socket));
    test_socket.write_buf.len = sizeof(test_socket.write_buf.buf);
    test_socket.write_buf.available_len = test_socket.write_buf.len;

    if (client) {
        client->get_time = test_time_get;
        client->read = test_socket_read;
        client->write = test_socket_write;
        client->data = &test_socket;
    }
}

static void test_socket_append(int val)
{
    test_buffer_t *buf = &test_socket.read_buf;

    buf->buf[test_socket.test_pos_read++] = val;
    buf->available_len += 1;
    buf->len += 1;
}

static int test_socket_shift()
{
    test_buffer_t *buf = &test_socket.write_buf;

    if (test_socket.test_pos_write < buf->pos)
        return buf->buf[test_socket.test_pos_write++];

    return -1;
}

static lmqtt_io_result_t read_buf(void *data, u8 *buf, int buf_len,
    int *bytes_read)
{
    test_buffer_t *source = (test_buffer_t *) data;

    return test_buffer_move(source, buf, &source->buf[source->pos], buf_len,
        bytes_read);
}

static lmqtt_io_result_t write_buf(void *data, u8 *buf, int buf_len,
    int *bytes_written)
{
    test_buffer_t *destination = (test_buffer_t *) data;

    return test_buffer_move(destination, &destination->buf[destination->pos],
        buf, buf_len, bytes_written);
}

static void on_connect(void *data)
{
    *((int *) data) = 1;
}

static int prepare_connection(lmqtt_client_t *client, long keep_alive)
{
    lmqtt_connect_t connect;

    lmqtt_client_initialize(client);
    test_socket_init_with_client(client);

    memset(&connect, 0, sizeof(connect));
    connect.keep_alive = keep_alive;

    lmqtt_client_connect(client, &connect);

    test_socket_append(TEST_CONNACK_SUCCESS);

    return LMQTT_IO_STATUS_READY == process_output(client) &&
        LMQTT_IO_STATUS_READY == process_input(client) &&
        TEST_CONNECT == test_socket_shift();
}

static void prepare_keep_alive_test(lmqtt_client_t *client, long keep_alive,
    long secs, long nsecs)
{
    lmqtt_client_initialize(client);

    client->get_time = test_time_get;
    client->internal.keep_alive = keep_alive;
    client->internal.last_resp.secs = secs;
    client->internal.last_resp.nsecs = nsecs;
}

START_TEST(should_initialize_client)
{
    lmqtt_client_t client;

    memset(&client, -1, sizeof(client));

    lmqtt_client_initialize(&client);

    ck_assert_ptr_eq(0, client.data);
    ck_assert(!client.read);
    ck_assert(!client.write);
    ck_assert_int_eq(0, client.failed);
}
END_TEST

START_TEST(should_prepare_connect_after_initialize)
{
    lmqtt_client_t client;
    lmqtt_connect_t connect;

    lmqtt_client_initialize(&client);
    test_socket_init_with_client(&client);

    memset(&connect, 0, sizeof(connect));
    ck_assert_int_eq(1, lmqtt_client_connect(&client, &connect));
    ck_assert_int_eq(LMQTT_IO_STATUS_READY, process_output(&client));

    ck_assert_int_eq(TEST_CONNECT, test_socket_shift());
}
END_TEST

START_TEST(should_not_prepare_connect_twice)
{
    lmqtt_client_t client;
    lmqtt_connect_t connect;

    lmqtt_client_initialize(&client);
    test_socket_init_with_client(&client);

    memset(&connect, 0, sizeof(connect));

    ck_assert_int_eq(1, lmqtt_client_connect(&client, &connect));
    process_output(&client);
    ck_assert_int_eq(TEST_CONNECT, test_socket_shift());

    ck_assert_int_eq(0, lmqtt_client_connect(&client, &connect));
    process_output(&client);
    ck_assert_int_eq(-1, test_socket_shift());
}
END_TEST

START_TEST(should_receive_connack_after_connect)
{
    lmqtt_client_t client;
    lmqtt_connect_t connect;
    int connected;

    lmqtt_client_initialize(&client);
    test_socket_init_with_client(&client);

    lmqtt_client_set_on_connect(&client, on_connect, &connected);

    lmqtt_client_connect(&client, &connect);
    process_output(&client);

    connected = 0;
    test_socket_append(TEST_CONNACK_SUCCESS);
    ck_assert_int_eq(LMQTT_IO_STATUS_READY, process_input(&client));
    ck_assert_int_eq(1, connected);

    /* should not receive connack twice */
    connected = 0;
    test_socket_append(TEST_CONNACK_SUCCESS);
    ck_assert_int_eq(LMQTT_IO_STATUS_ERROR, process_input(&client));
    ck_assert_int_eq(0, connected);
}
END_TEST

START_TEST(should_not_call_connect_callback_on_connect_failure)
{
    lmqtt_client_t client;
    lmqtt_connect_t connect;
    int connected;

    lmqtt_client_initialize(&client);
    test_socket_init_with_client(&client);

    lmqtt_client_set_on_connect(&client, on_connect, &connected);

    lmqtt_client_connect(&client, &connect);
    process_output(&client);

    connected = 0;
    test_socket_append(TEST_CONNACK_FAILURE);
    ck_assert_int_eq(LMQTT_IO_STATUS_ERROR, process_input(&client));
    ck_assert_int_eq(0, connected);
}
END_TEST

START_TEST(should_not_receive_connack_before_connect)
{
    lmqtt_client_t client;
    lmqtt_connect_t connect;
    int connected;

    lmqtt_client_initialize(&client);
    test_socket_init_with_client(&client);

    lmqtt_client_set_on_connect(&client, on_connect, &connected);

    connected = 0;
    test_socket_append(TEST_CONNACK_SUCCESS);
    ck_assert_int_eq(LMQTT_IO_STATUS_ERROR, process_input(&client));
    ck_assert_int_eq(0, connected);
}
END_TEST

START_TEST(should_get_integral_time_until_keep_alive)
{
    lmqtt_client_t client;
    long secs, nsecs;

    prepare_keep_alive_test(&client, 5, 10, 0);

    test_time_set(14, 0);
    ck_assert_int_eq(1, client_get_timeout_to(&client, 5, &secs, &nsecs));

    ck_assert_int_eq(1, secs);
    ck_assert_int_eq(0, nsecs);
}
END_TEST

START_TEST(should_get_fractional_time_until_keep_alive)
{
    lmqtt_client_t client;
    long secs, nsecs;

    prepare_keep_alive_test(&client, 5, 10, 500e6);

    test_time_set(14, 400e6);
    ck_assert_int_eq(1, client_get_timeout_to(&client, 5, &secs, &nsecs));

    ck_assert_int_eq(1, secs);
    ck_assert_int_eq(100e6, nsecs);
}
END_TEST

START_TEST(should_get_fractional_time_with_carry_until_keep_alive)
{
    lmqtt_client_t client;
    long secs, nsecs;

    prepare_keep_alive_test(&client, 5, 10, 500e6);

    test_time_set(14, 600e6);
    ck_assert_int_eq(1, client_get_timeout_to(&client, 5, &secs, &nsecs));

    ck_assert_int_eq(0, secs);
    ck_assert_int_eq(900e6, nsecs);
}
END_TEST

START_TEST(should_get_time_until_expired_keep_alive)
{
    lmqtt_client_t client;
    long secs, nsecs;

    prepare_keep_alive_test(&client, 5, 10, 500e6);

    test_time_set(15, 600e6);
    ck_assert_int_eq(1, client_get_timeout_to(&client, 5, &secs, &nsecs));

    ck_assert_int_eq(0, secs);
    ck_assert_int_eq(0, nsecs);
}
END_TEST

START_TEST(should_get_time_until_keep_alive_at_expiration_time)
{
    lmqtt_client_t client;
    long secs, nsecs;

    prepare_keep_alive_test(&client, 5, 10, 500e6);

    test_time_set(15, 500e6);
    ck_assert_int_eq(1, client_get_timeout_to(&client, 5, &secs, &nsecs));

    ck_assert_int_eq(0, secs);
    ck_assert_int_eq(0, nsecs);
}
END_TEST

START_TEST(should_get_time_with_zeroed_keep_alive)
{
    lmqtt_client_t client;
    long secs, nsecs;

    prepare_keep_alive_test(&client, 0, 10, 0);

    test_time_set(11, 0);
    ck_assert_int_eq(0, client_get_timeout_to(&client, 0, &secs, &nsecs));

    ck_assert_int_eq(0, secs);
    ck_assert_int_eq(0, nsecs);
}
END_TEST

START_TEST(should_send_pingreq_after_timeout)
{
    lmqtt_client_t client;
    long secs, nsecs;

    test_time_set(10, 0);
    ck_assert_int_eq(1, prepare_connection(&client, 5));

    test_time_set(15, 0);
    ck_assert_int_eq(1, lmqtt_client_get_timeout(&client, &secs, &nsecs));
    ck_assert_int_eq(0, secs);

    ck_assert_int_eq(LMQTT_IO_STATUS_READY, client_keep_alive(&client));
    ck_assert_int_eq(LMQTT_IO_STATUS_READY, process_output(&client));
    ck_assert_int_eq(TEST_PINGREQ, test_socket_shift());

    ck_assert_int_eq(1, lmqtt_client_get_timeout(&client, &secs, &nsecs));
    ck_assert_int_eq(5, secs);
}
END_TEST

START_TEST(should_not_send_pingreq_before_timeout)
{
    lmqtt_client_t client;
    long secs, nsecs;

    test_time_set(10, 0);
    ck_assert_int_eq(1, prepare_connection(&client, 5));

    test_time_set(14, 0);
    ck_assert_int_eq(1, lmqtt_client_get_timeout(&client, &secs, &nsecs));
    ck_assert_int_eq(1, secs);

    ck_assert_int_eq(LMQTT_IO_STATUS_READY, client_keep_alive(&client));
    ck_assert_int_eq(LMQTT_IO_STATUS_READY, process_output(&client));
    ck_assert_int_eq(-1, test_socket_shift());

    ck_assert_int_eq(1, lmqtt_client_get_timeout(&client, &secs, &nsecs));
    ck_assert_int_eq(1, secs);
}
END_TEST

START_TEST(should_not_send_pingreq_with_response_pending)
{
    lmqtt_client_t client;
    long secs, nsecs;

    test_time_set(10, 0);
    ck_assert_int_eq(1, prepare_connection(&client, 5));

    test_time_set(16, 0);
    client_keep_alive(&client);
    process_output(&client);
    ck_assert_int_eq(TEST_PINGREQ, test_socket_shift());

    test_time_set(17, 0);
    client_keep_alive(&client);
    process_output(&client);
    ck_assert_int_eq(-1, test_socket_shift());
}
END_TEST

START_TEST(should_send_pingreq_after_pingresp)
{
    lmqtt_client_t client;
    long secs, nsecs;

    test_time_set(10, 0);
    ck_assert_int_eq(1, prepare_connection(&client, 5));

    test_time_set(16, 0);
    client_keep_alive(&client);
    process_output(&client);
    ck_assert_int_eq(TEST_PINGREQ, test_socket_shift());
    test_socket_append(TEST_PINGRESP);
    process_input(&client);

    test_time_set(22, 0);
    client_keep_alive(&client);
    process_output(&client);
    ck_assert_int_eq(TEST_PINGREQ, test_socket_shift());
}
END_TEST

START_TEST(should_fail_client_after_pingreq_timeout)
{
    lmqtt_client_t client;
    long secs, nsecs;

    test_time_set(10, 0);
    ck_assert_int_eq(1, prepare_connection(&client, 5));

    test_time_set(15, 0);
    ck_assert_int_eq(1, lmqtt_client_get_timeout(&client, &secs, &nsecs));
    ck_assert_int_eq(0, secs);

    client_keep_alive(&client);
    process_output(&client);
    ck_assert_int_eq(TEST_PINGREQ, test_socket_shift());

    test_time_set(20, 0);
    ck_assert_int_eq(LMQTT_IO_STATUS_ERROR, client_keep_alive(&client));
    ck_assert_int_eq(LMQTT_IO_STATUS_ERROR, process_output(&client));
    ck_assert_int_eq(-1, test_socket_shift());
}
END_TEST

/* TODO: refactor duplicated code in the next 4 tests */
START_TEST(should_not_send_pingreq_after_failure)
{
    lmqtt_client_t client;
    lmqtt_connect_t connect;

    test_time_set(10, 0);
    lmqtt_client_initialize(&client);
    test_socket_init_with_client(&client);

    memset(&connect, 0, sizeof(connect));
    connect.keep_alive = 5;
    lmqtt_client_connect(&client, &connect);
    process_output(&client);
    ck_assert_int_eq(TEST_CONNECT, test_socket_shift());

    test_socket_append(TEST_CONNACK_FAILURE);
    ck_assert_int_eq(LMQTT_IO_STATUS_ERROR, process_input(&client));

    test_time_set(16, 0);
    ck_assert_int_eq(LMQTT_IO_STATUS_ERROR, client_keep_alive(&client));
    process_output(&client);
    ck_assert_int_eq(-1, test_socket_shift());
}
END_TEST

START_TEST(should_not_send_pingreq_before_connack)
{
    lmqtt_client_t client;
    lmqtt_connect_t connect;

    test_time_set(10, 0);
    lmqtt_client_initialize(&client);
    test_socket_init_with_client(&client);

    memset(&connect, 0, sizeof(connect));
    connect.keep_alive = 5;
    lmqtt_client_connect(&client, &connect);
    process_output(&client);
    ck_assert_int_eq(TEST_CONNECT, test_socket_shift());

    test_time_set(16, 0);
    ck_assert_int_eq(LMQTT_IO_STATUS_READY, client_keep_alive(&client));
    process_output(&client);
    ck_assert_int_eq(-1, test_socket_shift());
}
END_TEST

START_TEST(should_fail_client_after_connection_timeout)
{
    lmqtt_client_t client;
    lmqtt_connect_t connect;

    test_time_set(10, 0);
    lmqtt_client_initialize(&client);
    test_socket_init_with_client(&client);

    memset(&connect, 0, sizeof(connect));
    connect.keep_alive = 5;
    lmqtt_client_connect(&client, &connect);
    process_output(&client);
    ck_assert_int_eq(TEST_CONNECT, test_socket_shift());

    test_time_set(21, 0);
    ck_assert_int_eq(LMQTT_IO_STATUS_ERROR, client_keep_alive(&client));
    process_output(&client);
    ck_assert_int_eq(-1, test_socket_shift());
}
END_TEST

START_TEST(should_not_send_pingreq_with_zeroed_keep_alive)
{
    lmqtt_client_t client;
    lmqtt_connect_t connect;

    test_time_set(10, 0);
    lmqtt_client_initialize(&client);
    test_socket_init_with_client(&client);

    memset(&connect, 0, sizeof(connect));
    connect.keep_alive = 0;
    lmqtt_client_connect(&client, &connect);
    process_output(&client);
    ck_assert_int_eq(TEST_CONNECT, test_socket_shift());

    test_socket_append(TEST_CONNACK_SUCCESS);
    ck_assert_int_eq(LMQTT_IO_STATUS_READY, process_input(&client));

    test_time_set(11, 0);
    ck_assert_int_eq(LMQTT_IO_STATUS_READY, client_keep_alive(&client));
    process_output(&client);
    ck_assert_int_eq(-1, test_socket_shift());
}
END_TEST

START_TCASE("Client initialize")
{
    ADD_TEST(should_initialize_client);
    ADD_TEST(should_prepare_connect_after_initialize);
    ADD_TEST(should_not_prepare_connect_twice);
    ADD_TEST(should_receive_connack_after_connect);
    ADD_TEST(should_not_call_connect_callback_on_connect_failure);
    ADD_TEST(should_not_receive_connack_before_connect);

    ADD_TEST(should_get_integral_time_until_keep_alive);
    ADD_TEST(should_get_fractional_time_until_keep_alive);
    ADD_TEST(should_get_fractional_time_with_carry_until_keep_alive);
    ADD_TEST(should_get_time_until_expired_keep_alive);
    ADD_TEST(should_get_time_until_keep_alive_at_expiration_time);
    ADD_TEST(should_get_time_with_zeroed_keep_alive);

    ADD_TEST(should_send_pingreq_after_timeout);
    ADD_TEST(should_not_send_pingreq_before_timeout);
    ADD_TEST(should_not_send_pingreq_with_response_pending);
    ADD_TEST(should_send_pingreq_after_pingresp);
    ADD_TEST(should_fail_client_after_pingreq_timeout);
    ADD_TEST(should_not_send_pingreq_after_failure);
    ADD_TEST(should_not_send_pingreq_before_connack);
    ADD_TEST(should_fail_client_after_connection_timeout);
    ADD_TEST(should_not_send_pingreq_with_zeroed_keep_alive);
}
END_TCASE