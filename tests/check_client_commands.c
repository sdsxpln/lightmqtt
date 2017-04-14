#include "check_lightmqtt.h"
#include <assert.h>

#include "../src/lmqtt_io.c"

typedef enum {
    TEST_CONNECT = 5000,
    TEST_SUBSCRIBE,
    TEST_UNSUBSCRIBE,
    TEST_PUBLISH,
    TEST_PUBREL,
    TEST_PINGREQ,
    TEST_DISCONNECT
} test_type_request_t;

typedef enum {
    TEST_CONNACK_SUCCESS = 5100,
    TEST_CONNACK_FAILURE,
    TEST_SUBACK_SUCCESS,
    TEST_UNSUBACK_SUCCESS,
    TEST_PUBACK,
    TEST_PUBREC,
    TEST_PUBCOMP,
    TEST_PINGRESP
} test_type_response_t;

typedef struct {
    test_buffer_t read_buf;
    test_buffer_t write_buf;
    int test_pos_read;
    int test_pos_write;
} test_socket_t;

typedef struct {
    void *data;
    int succeeded;
} test_cb_result_t;

static test_socket_t test_socket;

static lmqtt_io_result_t test_socket_read(void *data, u8 *buf, int buf_len,
    int *bytes_read)
{
    test_socket_t *sock = (test_socket_t *) data;
    return test_buffer_read(&sock->read_buf, buf, buf_len, bytes_read);
}

static lmqtt_io_result_t test_socket_write(void *data, u8 *buf, int buf_len,
    int *bytes_written)
{
    test_socket_t *sock = (test_socket_t *) data;
    return test_buffer_write(&sock->write_buf, buf, buf_len, bytes_written);
}

static void test_socket_init_with_client(lmqtt_client_t *client)
{
    memset(&test_socket, 0, sizeof(test_socket));
    test_socket.read_buf.len = sizeof(test_socket.read_buf.buf);
    test_socket.write_buf.len = sizeof(test_socket.write_buf.buf);
    test_socket.write_buf.available_len = test_socket.write_buf.len;

    if (client) {
        client->get_time = test_time_get;
        client->connect_store.get_time = test_time_get;
        client->main_store.get_time = test_time_get;
        client->read = test_socket_read;
        client->write = test_socket_write;
        client->data = &test_socket;
    }
}

static void test_socket_append_param(int val, int param)
{
    test_buffer_t *buf = &test_socket.read_buf;

    char src[5];
    int len = 0;

    switch (val) {
        case TEST_CONNACK_SUCCESS:
            memcpy(src, "\x20\x02\x00\x00", 4);
            len = 4;
            break;
        case TEST_CONNACK_FAILURE:
            memcpy(src, "\x20\x02\x00\x01", 4);
            len = 4;
            break;
        case TEST_SUBACK_SUCCESS:
            memcpy(src, "\x90\x03\x00\x00\x00", 5);
            src[2] = param >> 8;
            src[3] = param;
            len = 5;
            break;
        case TEST_UNSUBACK_SUCCESS:
            memcpy(src, "\xb0\x02\x00\x00", 4);
            src[2] = param >> 8;
            src[3] = param;
            len = 4;
            break;
        case TEST_PUBACK:
            memcpy(src, "\x40\x02\x00\x00", 4);
            src[2] = param >> 8;
            src[3] = param;
            len = 4;
            break;
        case TEST_PUBREC:
            memcpy(src, "\x50\x02\x00\x00", 4);
            src[2] = param >> 8;
            src[3] = param;
            len = 4;
            break;
        case TEST_PUBCOMP:
            memcpy(src, "\x70\x02\x00\x00", 4);
            src[2] = param >> 8;
            src[3] = param;
            len = 4;
            break;
        case TEST_PINGRESP:
            memcpy(src, "\xd0\x00", 2);
            len = 2;
            break;
    }

    if (src) {
        memcpy(&buf->buf[test_socket.test_pos_read], src, len);
        test_socket.test_pos_read += len;
        buf->available_len += len;
    }
}

static void test_socket_append(int val)
{
    test_socket_append_param(val, 0);
}

static int test_socket_shift()
{
    test_buffer_t *buf = &test_socket.write_buf;

    test_type_request_t result = -4; /* invalid command */
    u8 cmd;
    u8 remain_len;
    int len;
    int available = buf->pos - test_socket.test_pos_write;

    if (available <= 0)
        return -1; /* eof */
    if (available < 2)
        return -2; /* partial buffer */

    cmd = buf->buf[test_socket.test_pos_write];
    remain_len = buf->buf[test_socket.test_pos_write + 1];

    if (remain_len & 0x80)
        return -3; /* invalid remaining length (for simplicity we do not
                      implement here decoding for values larger than 127) */

    len = 2 + remain_len;
    if (available < len)
        return -2; /* partial buffer */

    switch (cmd & 0xf0) {
        case 0x10:
            result = TEST_CONNECT;
            break;
        case 0x80:
            result = TEST_SUBSCRIBE;
            break;
        case 0xa0:
            result = TEST_UNSUBSCRIBE;
            break;
        case 0x30:
            result = TEST_PUBLISH;
            break;
        case 0x60:
            result = TEST_PUBREL;
            break;
        case 0xc0:
            result = TEST_PINGREQ;
            break;
        case 0xe0:
            result = TEST_DISCONNECT;
            break;
    }

    test_socket.test_pos_write += len;
    return result;
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

static void test_cb_result_set(void *cb_result, void *data, int succeeded)
{
    test_cb_result_t *result = (test_cb_result_t *) cb_result;
    result->data = data;
    result->succeeded = succeeded;
}

static void on_connect(void *data, lmqtt_connect_t *connect, int succeeded)
{
    test_cb_result_set(data, connect, succeeded);
}

static void on_subscribe(void *data, lmqtt_subscribe_t *subscribe, int succeeded)
{
    test_cb_result_set(data, subscribe, succeeded);
}

static void on_unsubscribe(void *data, lmqtt_subscribe_t *subscribe, int succeeded)
{
    test_cb_result_set(data, subscribe, succeeded);
}

static void on_publish(void *data, lmqtt_publish_t *publish, int succeeded)
{
    test_cb_result_set(data, publish, succeeded);
}

static lmqtt_connect_t connect;
static lmqtt_publish_t publish;

static void do_init(lmqtt_client_t *client, long timeout)
{
    lmqtt_client_initialize(client);
    lmqtt_client_set_default_timeout(client, timeout);
    test_socket_init_with_client(client);
}

static int do_connect(lmqtt_client_t *client, long keep_alive)
{
    memset(&connect, 0, sizeof(connect));
    connect.keep_alive = keep_alive;
    connect.clean_session = 0;
    connect.client_id.buf = "test";
    connect.client_id.len = 4;

    return lmqtt_client_connect(client, &connect);
}

static int do_connect_connack_process(lmqtt_client_t *client, long keep_alive)
{
    do_connect(client, keep_alive);
    test_socket_append(TEST_CONNACK_SUCCESS);

    return LMQTT_IO_STATUS_BLOCK_DATA == client_process_output(client) &&
        LMQTT_IO_STATUS_BLOCK_CONN == client_process_input(client) &&
        TEST_CONNECT == test_socket_shift();
}

static int do_init_connect_connack_process(lmqtt_client_t *client,
    long keep_alive, long timeout)
{
    do_init(client, timeout);
    return do_connect_connack_process(client, keep_alive);
}

static int do_connect_process(lmqtt_client_t *client, long keep_alive)
{
    do_connect(client, keep_alive);

    return LMQTT_IO_STATUS_BLOCK_DATA == client_process_output(client) &&
        TEST_CONNECT == test_socket_shift();
}

static int do_init_connect_process(lmqtt_client_t *client, long keep_alive,
    long timeout)
{
    do_init(client, timeout);
    return do_connect_process(client, keep_alive);
}

static int do_publish(lmqtt_client_t *client, int qos)
{
    memset(&publish, 0, sizeof(publish));
    publish.qos = qos;
    publish.topic.buf = "topic";
    publish.topic.len = strlen(publish.topic.buf);
    publish.payload.buf = "payload";
    publish.payload.len = strlen(publish.payload.buf);

    return lmqtt_client_publish(client, &publish);
}

static int close_read_buf(lmqtt_client_t *client)
{
    int previous_len = test_socket.read_buf.len;
    int result;

    test_socket.read_buf.len = test_socket.read_buf.available_len;
    result = LMQTT_IO_STATUS_READY == client_process_input(client);
    test_socket.read_buf.len = previous_len;

    return result;
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
    ck_assert_ptr_eq(&client.connect_store, client.rx_state.store);
    ck_assert_ptr_eq(&client.connect_store, client.tx_state.store);
}
END_TEST

START_TEST(should_prepare_connect_after_initialize)
{
    lmqtt_client_t client;
    lmqtt_connect_t connect;

    do_init(&client, 5);

    memset(&connect, 0, sizeof(connect));
    connect.clean_session = 1;

    ck_assert_int_eq(1, lmqtt_client_connect(&client, &connect));
    ck_assert_int_eq(LMQTT_IO_STATUS_BLOCK_DATA, client_process_output(&client));
    ck_assert_int_eq(TEST_CONNECT, test_socket_shift());
}
END_TEST

START_TEST(should_not_prepare_connect_twice)
{
    lmqtt_client_t client;
    lmqtt_connect_t connect;

    do_init(&client, 5);

    memset(&connect, 0, sizeof(connect));
    connect.clean_session = 1;

    ck_assert_int_eq(1, lmqtt_client_connect(&client, &connect));
    client_process_output(&client);
    ck_assert_int_eq(TEST_CONNECT, test_socket_shift());

    ck_assert_int_eq(0, lmqtt_client_connect(&client, &connect));
    client_process_output(&client);
    ck_assert_int_eq(-1, test_socket_shift());
}
END_TEST

START_TEST(should_not_prepare_invalid_connect)
{
    lmqtt_client_t client;
    lmqtt_connect_t connect;

    do_init(&client, 5);

    memset(&connect, 0, sizeof(connect));
    connect.clean_session = 0; /* invalid; should have a non-empty client_id */

    ck_assert_int_eq(0, lmqtt_client_connect(&client, &connect));
    ck_assert_int_eq(LMQTT_IO_STATUS_BLOCK_DATA, client_process_output(&client));
    ck_assert_int_eq(-1, test_socket_shift());
}
END_TEST

START_TEST(should_receive_connack_after_connect)
{
    lmqtt_client_t client;
    lmqtt_connect_t connect;
    test_cb_result_t cb_result = { 0, 0 };

    do_init(&client, 5);

    lmqtt_client_set_on_connect(&client, on_connect, &cb_result);

    memset(&connect, 0, sizeof(connect));
    connect.clean_session = 1;
    lmqtt_client_connect(&client, &connect);
    client_process_output(&client);

    test_socket_append(TEST_CONNACK_SUCCESS);
    ck_assert_int_eq(LMQTT_IO_STATUS_BLOCK_CONN, client_process_input(&client));
    ck_assert_ptr_eq(&connect, cb_result.data);
    ck_assert_int_eq(1, cb_result.succeeded);

    /* should not receive connack twice */
    cb_result.data = 0;
    test_socket_append(TEST_CONNACK_SUCCESS);
    ck_assert_int_eq(LMQTT_IO_STATUS_ERROR, client_process_input(&client));
    ck_assert_ptr_eq(0, cb_result.data);
}
END_TEST

START_TEST(should_call_connect_callback_on_connect_failure)
{
    lmqtt_client_t client;
    lmqtt_connect_t connect;
    test_cb_result_t cb_result = { 0, 0 };

    do_init(&client, 5);

    lmqtt_client_set_on_connect(&client, on_connect, &cb_result);

    memset(&connect, 0, sizeof(connect));
    connect.clean_session = 1;
    lmqtt_client_connect(&client, &connect);
    client_process_output(&client);

    cb_result.data = 0;
    test_socket_append(TEST_CONNACK_FAILURE);
    ck_assert_int_eq(LMQTT_IO_STATUS_BLOCK_CONN, client_process_input(&client));

    ck_assert_ptr_eq(&connect, cb_result.data);
    ck_assert_int_eq(0, cb_result.succeeded);
    ck_assert_int_eq(LMQTT_IO_STATUS_READY, client_process_output(&client));
}
END_TEST

START_TEST(should_not_receive_connack_before_connect)
{
    lmqtt_client_t client;
    lmqtt_connect_t connect;
    test_cb_result_t cb_result = { 0, 0 };

    do_init(&client, 5);

    memset(&connect, 0, sizeof(connect));
    connect.clean_session = 1;
    lmqtt_client_set_on_connect(&client, on_connect, &cb_result);

    cb_result.data = 0;
    test_socket_append(TEST_CONNACK_SUCCESS);
    ck_assert_int_eq(LMQTT_IO_STATUS_ERROR, client_process_input(&client));
    ck_assert_ptr_eq(0, cb_result.data);
}
END_TEST

START_TEST(should_subscribe)
{
    lmqtt_client_t client;
    lmqtt_subscribe_t subscribe;
    lmqtt_subscription_t subscription;
    test_cb_result_t cb_result = { 0, 0 };

    ck_assert_int_eq(1, do_init_connect_connack_process(&client, 5, 3));

    lmqtt_client_set_on_subscribe(&client, on_subscribe, &cb_result);

    memset(&subscribe, 0, sizeof(subscribe));
    memset(&subscription, 0, sizeof(subscription));
    subscribe.count = 1;
    subscribe.subscriptions = &subscription;
    subscription.qos = 0;
    subscription.topic.buf = "test";
    subscription.topic.len = strlen(subscription.topic.buf);

    ck_assert_int_eq(1, lmqtt_client_subscribe(&client, &subscribe));
    ck_assert_int_eq(LMQTT_IO_STATUS_BLOCK_DATA, client_process_output(&client));
    ck_assert_int_eq(TEST_SUBSCRIBE, test_socket_shift());

    test_socket_append(TEST_SUBACK_SUCCESS);
    ck_assert_int_eq(LMQTT_IO_STATUS_BLOCK_CONN, client_process_input(&client));
    ck_assert_ptr_eq(&subscribe, cb_result.data);
    ck_assert_int_eq(1, cb_result.succeeded);
}
END_TEST

START_TEST(should_unsubscribe)
{
    lmqtt_client_t client;
    lmqtt_subscribe_t subscribe;
    lmqtt_subscription_t subscription;
    test_cb_result_t cb_result = { 0, 0 };

    ck_assert_int_eq(1, do_init_connect_connack_process(&client, 5, 3));

    lmqtt_client_set_on_unsubscribe(&client, on_unsubscribe, &cb_result);

    memset(&subscribe, 0, sizeof(subscribe));
    memset(&subscription, 0, sizeof(subscription));
    subscribe.count = 1;
    subscribe.subscriptions = &subscription;
    subscription.topic.buf = "test";
    subscription.topic.len = strlen(subscription.topic.buf);

    ck_assert_int_eq(1, lmqtt_client_unsubscribe(&client, &subscribe));
    ck_assert_int_eq(LMQTT_IO_STATUS_BLOCK_DATA, client_process_output(&client));
    ck_assert_int_eq(TEST_UNSUBSCRIBE, test_socket_shift());

    test_socket_append(TEST_UNSUBACK_SUCCESS);
    ck_assert_int_eq(LMQTT_IO_STATUS_BLOCK_CONN, client_process_input(&client));
    ck_assert_ptr_eq(&subscribe, cb_result.data);
    ck_assert_int_eq(1, cb_result.succeeded);
}
END_TEST

START_TEST(should_assign_packet_ids_to_subscribe)
{
    lmqtt_client_t client;
    lmqtt_subscribe_t subscribe[3];
    lmqtt_subscription_t subscriptions[3];
    void *data;
    lmqtt_store_entry_callback_t cb;
    int i;

    memset(&subscribe, 0, sizeof(subscribe));
    memset(&subscriptions, 0, sizeof(subscriptions));
    for (i = 0; i < 3; i++) {
        subscribe[i].count = 1;
        subscribe[i].subscriptions = &subscriptions[i];
        subscriptions[i].topic.buf = "test";
        subscriptions[i].topic.len = strlen(subscriptions[i].topic.buf);
    }

    ck_assert_int_eq(1, do_init_connect_connack_process(&client, 5, 3));

    ck_assert_int_eq(1, lmqtt_client_subscribe(&client, &subscribe[0]));
    ck_assert_int_eq(1, lmqtt_client_subscribe(&client, &subscribe[1]));
    ck_assert_int_eq(1, lmqtt_client_subscribe(&client, &subscribe[2]));

    ck_assert_uint_eq(0, subscribe[0].packet_id);
    ck_assert_uint_eq(1, subscribe[1].packet_id);
    ck_assert_uint_eq(2, subscribe[2].packet_id);

    lmqtt_store_mark_current(&client.main_store);
    lmqtt_store_mark_current(&client.main_store);
    lmqtt_store_mark_current(&client.main_store);

    ck_assert_int_eq(1, lmqtt_store_pop_marked_by(&client.main_store,
        LMQTT_CLASS_SUBSCRIBE, 0, NULL));
    ck_assert_int_eq(1, lmqtt_store_pop_marked_by(&client.main_store,
        LMQTT_CLASS_SUBSCRIBE, 1, NULL));
    ck_assert_int_eq(1, lmqtt_store_pop_marked_by(&client.main_store,
        LMQTT_CLASS_SUBSCRIBE, 2, NULL));
}
END_TEST

START_TEST(should_not_subscribe_with_invalid_packet)
{
    lmqtt_client_t client;
    lmqtt_subscribe_t subscribe;

    ck_assert_int_eq(1, do_init_connect_connack_process(&client, 5, 3));

    memset(&subscribe, 0, sizeof(subscribe));

    ck_assert_int_eq(0, lmqtt_client_subscribe(&client, &subscribe));
    ck_assert_int_eq(LMQTT_IO_STATUS_BLOCK_DATA, client_process_output(&client));
    ck_assert_int_eq(-1, test_socket_shift());
}
END_TEST

START_TEST(should_not_subscribe_with_full_store)
{
    lmqtt_client_t client;
    lmqtt_subscribe_t subscribe;
    lmqtt_subscription_t subscription;
    int i;

    ck_assert_int_eq(1, do_init_connect_connack_process(&client, 5, 3));

    memset(&subscribe, 0, sizeof(subscribe));
    memset(&subscription, 0, sizeof(subscription));
    subscribe.count = 1;
    subscribe.subscriptions = &subscription;
    subscription.qos = 0;
    subscription.topic.buf = "test";
    subscription.topic.len = strlen(subscription.topic.buf);

    for (i = 0; lmqtt_store_append(&client.main_store, LMQTT_CLASS_PINGREQ, 0,
            NULL); i++)
        ;

    ck_assert_int_eq(0, lmqtt_client_subscribe(&client, &subscribe));
}
END_TEST

START_TEST(should_publish_with_qos_0)
{
    lmqtt_client_t client;
    test_cb_result_t cb_result = { 0, 0 };

    ck_assert_int_eq(1, do_init_connect_connack_process(&client, 5, 3));

    lmqtt_client_set_on_publish(&client, on_publish, &cb_result);
    lmqtt_store_get_id(&client.main_store);
    ck_assert_int_eq(1, do_publish(&client, 0));
    ck_assert_int_eq(LMQTT_IO_STATUS_BLOCK_DATA, client_process_output(&client));
    ck_assert_int_eq(TEST_PUBLISH, test_socket_shift());

    /* should NOT assign id to QoS 0 packet */
    ck_assert_int_eq(0, publish.packet_id);
    ck_assert_ptr_eq(&publish, cb_result.data);
    ck_assert_int_eq(1, cb_result.succeeded);
}
END_TEST

START_TEST(should_publish_with_qos_1)
{
    lmqtt_client_t client;
    test_cb_result_t cb_result = { 0, 0 };

    ck_assert_int_eq(1, do_init_connect_connack_process(&client, 5, 3));

    lmqtt_client_set_on_publish(&client, on_publish, &cb_result);
    lmqtt_store_get_id(&client.main_store);
    ck_assert_int_eq(1, do_publish(&client, 1));
    ck_assert_int_eq(LMQTT_IO_STATUS_BLOCK_DATA, client_process_output(&client));
    ck_assert_int_eq(TEST_PUBLISH, test_socket_shift());

    /* should assign id to QoS 1 packet */
    ck_assert_int_eq(1, publish.packet_id);
    ck_assert_ptr_eq(0, cb_result.data);
    ck_assert_int_eq(0, cb_result.succeeded);

    test_socket_append_param(TEST_PUBACK, 1);
    ck_assert_int_eq(LMQTT_IO_STATUS_BLOCK_CONN, client_process_input(&client));

    ck_assert_ptr_eq(&publish, cb_result.data);
    ck_assert_int_eq(1, cb_result.succeeded);
}
END_TEST

START_TEST(should_publish_with_qos_2)
{
    lmqtt_client_t client;
    test_cb_result_t cb_result = { 0, 0 };

    ck_assert_int_eq(1, do_init_connect_connack_process(&client, 5, 3));

    lmqtt_client_set_on_publish(&client, on_publish, &cb_result);

    lmqtt_store_get_id(&client.main_store);
    ck_assert_int_eq(1, do_publish(&client, 2));
    ck_assert_int_eq(LMQTT_IO_STATUS_BLOCK_DATA, client_process_output(&client));
    ck_assert_int_eq(TEST_PUBLISH, test_socket_shift());

    /* should assign id to QoS 2 packet */
    ck_assert_int_eq(1, publish.packet_id);
    ck_assert_ptr_eq(0, cb_result.data);
    ck_assert_int_eq(0, cb_result.succeeded);

    test_socket_append_param(TEST_PUBREC, 1);
    ck_assert_int_eq(LMQTT_IO_STATUS_BLOCK_CONN, client_process_input(&client));

    ck_assert_int_eq(LMQTT_IO_STATUS_BLOCK_DATA, client_process_output(&client));
    ck_assert_int_eq(TEST_PUBREL, test_socket_shift());

    ck_assert_ptr_eq(0, cb_result.data);
    ck_assert_int_eq(0, cb_result.succeeded);

    test_socket_append_param(TEST_PUBCOMP, 1);
    ck_assert_int_eq(LMQTT_IO_STATUS_BLOCK_CONN, client_process_input(&client));

    ck_assert_ptr_eq(&publish, cb_result.data);
    ck_assert_int_eq(1, cb_result.succeeded);
}
END_TEST

START_TEST(should_not_publish_invalid_packet)
{
    lmqtt_client_t client;
    lmqtt_publish_t publish;

    ck_assert_int_eq(1, do_init_connect_connack_process(&client, 5, 3));

    memset(&publish, 0, sizeof(publish));

    ck_assert_int_eq(0, lmqtt_client_publish(&client, &publish));
    ck_assert_int_eq(LMQTT_IO_STATUS_BLOCK_DATA, client_process_output(&client));
    ck_assert_int_eq(-1, test_socket_shift());
}
END_TEST

START_TEST(should_send_pingreq_after_timeout)
{
    lmqtt_client_t client;
    long secs, nsecs;

    test_time_set(10, 0);
    ck_assert_int_eq(1, do_init_connect_connack_process(&client, 5, 3));

    test_time_set(15, 0);
    ck_assert_int_eq(1, lmqtt_client_get_timeout(&client, &secs, &nsecs));
    ck_assert_int_eq(0, secs);

    ck_assert_int_eq(LMQTT_IO_STATUS_READY, client_keep_alive(&client));
    ck_assert_int_eq(LMQTT_IO_STATUS_BLOCK_DATA, client_process_output(&client));
    ck_assert_int_eq(TEST_PINGREQ, test_socket_shift());

    ck_assert_int_eq(1, lmqtt_client_get_timeout(&client, &secs, &nsecs));
    ck_assert_int_eq(3, secs);
}
END_TEST

START_TEST(should_not_send_pingreq_before_timeout)
{
    lmqtt_client_t client;
    long secs, nsecs;

    test_time_set(10, 0);
    ck_assert_int_eq(1, do_init_connect_connack_process(&client, 5, 3));

    test_time_set(14, 0);
    ck_assert_int_eq(1, lmqtt_client_get_timeout(&client, &secs, &nsecs));
    ck_assert_int_eq(1, secs);

    ck_assert_int_eq(LMQTT_IO_STATUS_READY, client_keep_alive(&client));
    ck_assert_int_eq(LMQTT_IO_STATUS_BLOCK_DATA, client_process_output(&client));
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
    ck_assert_int_eq(1, do_init_connect_connack_process(&client, 5, 3));

    test_time_set(16, 0);
    client_keep_alive(&client);
    client_process_output(&client);
    ck_assert_int_eq(TEST_PINGREQ, test_socket_shift());

    test_time_set(17, 0);
    client_keep_alive(&client);
    client_process_output(&client);
    ck_assert_int_eq(-1, test_socket_shift());
}
END_TEST

START_TEST(should_send_pingreq_after_pingresp)
{
    lmqtt_client_t client;
    long secs, nsecs;

    test_time_set(10, 0);
    ck_assert_int_eq(1, do_init_connect_connack_process(&client, 5, 3));

    test_time_set(16, 0);
    client_keep_alive(&client);
    client_process_output(&client);
    ck_assert_int_eq(TEST_PINGREQ, test_socket_shift());
    test_socket_append(TEST_PINGRESP);
    client_process_input(&client);

    test_time_set(22, 0);
    client_keep_alive(&client);
    client_process_output(&client);
    ck_assert_int_eq(TEST_PINGREQ, test_socket_shift());
}
END_TEST

START_TEST(should_fail_client_after_pingreq_timeout)
{
    lmqtt_client_t client;
    long secs, nsecs;

    test_time_set(10, 0);
    ck_assert_int_eq(1, do_init_connect_connack_process(&client, 5, 3));

    test_time_set(15, 0);
    ck_assert_int_eq(1, lmqtt_client_get_timeout(&client, &secs, &nsecs));
    ck_assert_int_eq(0, secs);

    client_keep_alive(&client);
    client_process_output(&client);
    ck_assert_int_eq(TEST_PINGREQ, test_socket_shift());

    test_time_set(20, 0);
    ck_assert_int_eq(LMQTT_IO_STATUS_ERROR, client_keep_alive(&client));
    ck_assert_int_eq(LMQTT_IO_STATUS_ERROR, client_process_output(&client));
    ck_assert_int_eq(-1, test_socket_shift());
}
END_TEST

START_TEST(should_disconnect)
{
    lmqtt_client_t client;

    ck_assert_int_eq(1, do_init_connect_connack_process(&client, 5, 3));

    ck_assert_int_eq(1, lmqtt_client_disconnect(&client));
    ck_assert_int_eq(LMQTT_IO_STATUS_READY, client_process_output(&client));
    ck_assert_int_eq(TEST_DISCONNECT, test_socket_shift());
}
END_TEST

START_TEST(should_not_send_pingreq_after_disconnect)
{
    lmqtt_client_t client;

    test_time_set(10, 0);
    do_init_connect_connack_process(&client, 5, 3);

    test_time_set(16, 0);
    ck_assert_int_eq(1, lmqtt_client_disconnect(&client));
    ck_assert_int_eq(LMQTT_IO_STATUS_READY, client_keep_alive(&client));

    ck_assert_int_eq(LMQTT_IO_STATUS_READY, client_process_output(&client));
    ck_assert_int_eq(TEST_DISCONNECT, test_socket_shift());
    ck_assert_int_eq(-1, test_socket_shift());
}
END_TEST

START_TEST(should_not_disconnect_before_connack)
{
    lmqtt_client_t client;

    do_init_connect_process(&client, 5, 3);

    ck_assert_int_eq(0, lmqtt_client_disconnect(&client));
    ck_assert_int_eq(LMQTT_IO_STATUS_BLOCK_DATA, client_process_output(&client));
    ck_assert_int_eq(-1, test_socket_shift());
}
END_TEST

START_TEST(should_not_send_pingreq_after_failure)
{
    lmqtt_client_t client;

    test_time_set(10, 0);
    do_init_connect_process(&client, 5, 0);

    test_socket_append(TEST_SUBACK_SUCCESS);
    ck_assert_int_eq(LMQTT_IO_STATUS_ERROR, client_process_input(&client));

    test_time_set(16, 0);
    ck_assert_int_eq(LMQTT_IO_STATUS_ERROR, client_keep_alive(&client));
    client_process_output(&client);
    ck_assert_int_eq(-1, test_socket_shift());
}
END_TEST

START_TEST(should_not_send_pingreq_before_connack)
{
    lmqtt_client_t client;

    test_time_set(10, 0);
    do_init_connect_process(&client, 5, 0);

    test_time_set(16, 0);
    ck_assert_int_eq(LMQTT_IO_STATUS_READY, client_keep_alive(&client));
    client_process_output(&client);
    ck_assert_int_eq(-1, test_socket_shift());
}
END_TEST

START_TEST(should_fail_client_after_connection_timeout)
{
    lmqtt_client_t client;

    test_time_set(10, 0);
    do_init_connect_process(&client, 5, 3);

    test_time_set(14, 0);
    ck_assert_int_eq(LMQTT_IO_STATUS_ERROR, client_keep_alive(&client));
    client_process_output(&client);
    ck_assert_int_eq(-1, test_socket_shift());
}
END_TEST

START_TEST(should_not_fail_client_before_connection_timeout)
{
    lmqtt_client_t client;

    test_time_set(10, 0);
    do_init_connect_process(&client, 5, 7);

    test_time_set(16, 0);
    ck_assert_int_eq(LMQTT_IO_STATUS_READY, client_keep_alive(&client));
    client_process_output(&client);
    ck_assert_int_eq(-1, test_socket_shift());
}
END_TEST

START_TEST(should_not_send_pingreq_with_zeroed_keep_alive)
{
    lmqtt_client_t client;

    test_time_set(10, 0);
    do_init_connect_process(&client, 0, 0);

    test_socket_append(TEST_CONNACK_SUCCESS);
    ck_assert_int_eq(LMQTT_IO_STATUS_BLOCK_CONN, client_process_input(&client));

    test_time_set(11, 0);
    ck_assert_int_eq(LMQTT_IO_STATUS_READY, client_keep_alive(&client));
    client_process_output(&client);
    ck_assert_int_eq(-1, test_socket_shift());
}
END_TEST

START_TEST(should_clean_pingreq_and_disconnect_packets_after_close)
{
    lmqtt_client_t client;
    int class;
    lmqtt_store_value_t value;

    test_time_set(10, 0);
    do_init_connect_connack_process(&client, 5, 3);

    test_time_set(16, 0);

    ck_assert_int_eq(LMQTT_IO_STATUS_READY, client_keep_alive(&client));
    ck_assert_int_eq(1, do_publish(&client, 0));
    ck_assert_int_eq(1, lmqtt_client_disconnect(&client));
    ck_assert_int_eq(3, lmqtt_store_count(&client.main_store));

    test_socket.read_buf.len = 0;
    ck_assert_int_eq(LMQTT_IO_STATUS_READY, client_process_input(&client));

    ck_assert_int_eq(1, lmqtt_store_count(&client.main_store));
    lmqtt_store_shift(&client.main_store, &class, &value);
    ck_assert_int_eq(LMQTT_CLASS_PUBLISH_0, class);
    ck_assert_ptr_eq(&publish, value.value);
}
END_TEST

START_TEST(should_close_encoder_after_socket_close)
{
    lmqtt_client_t client;

    do_init_connect_connack_process(&client, 5, 3);
    ck_assert_int_eq(1, do_publish(&client, 0));

    test_socket.read_buf.len = 0;
    ck_assert_int_eq(LMQTT_IO_STATUS_READY, client_process_input(&client));

    ck_assert_int_eq(LMQTT_IO_STATUS_READY, client_process_output(&client));
    ck_assert_int_eq(-1, test_socket_shift());
    ck_assert_int_eq(1, lmqtt_store_count(&client.main_store));
}
END_TEST

START_TEST(should_close_encoder_after_connect_failure)
{
    lmqtt_client_t client;

    do_init_connect_process(&client, 5, 3);

    test_socket_append(TEST_CONNACK_FAILURE);

    ck_assert_int_eq(LMQTT_IO_STATUS_BLOCK_DATA, client_process_output(&client));
    ck_assert_int_eq(LMQTT_IO_STATUS_BLOCK_CONN, client_process_input(&client));
    ck_assert_int_eq(LMQTT_IO_STATUS_READY, client_process_output(&client));
}
END_TEST

START_TEST(should_reconnect_after_close)
{
    lmqtt_client_t client;

    do_init_connect_connack_process(&client, 5, 3);

    test_socket.read_buf.len = 0;
    ck_assert_int_eq(LMQTT_IO_STATUS_READY, client_process_input(&client));

    test_socket.read_buf.len = sizeof(test_socket.read_buf.buf);
    ck_assert_int_eq(1, do_connect_connack_process(&client, 5));
}
END_TEST

START_TEST(should_reconnect_after_disconnect)
{
    lmqtt_client_t client;

    do_init_connect_connack_process(&client, 5, 3);

    ck_assert_int_eq(1, lmqtt_client_disconnect(&client));
    ck_assert_int_eq(LMQTT_IO_STATUS_READY, client_process_output(&client));
    ck_assert_int_eq(TEST_DISCONNECT, test_socket_shift());

    ck_assert_int_eq(1, do_connect_connack_process(&client, 5));
}
END_TEST

START_TEST(should_reset_output_buffer_on_reconnect)
{
    lmqtt_client_t client;

    do_init_connect_connack_process(&client, 5, 3);
    ck_assert_int_eq(1, do_publish(&client, 0));

    /* Fill tx buffer, but do not flush it */
    test_socket.write_buf.available_len = test_socket.write_buf.pos;
    ck_assert_int_eq(LMQTT_IO_STATUS_BLOCK_CONN, client_process_output(&client));

    /* Close remote side of connection */
    test_socket.read_buf.len = 0;
    ck_assert_int_eq(LMQTT_IO_STATUS_READY, client_process_input(&client));

    memset(&connect, 0, sizeof(connect));
    connect.client_id.buf = "test";
    connect.client_id.len = 4;
    ck_assert_int_eq(1, lmqtt_client_connect(&client, &connect));

    test_socket.write_buf.available_len = test_socket.write_buf.len;
    ck_assert_int_eq(LMQTT_IO_STATUS_BLOCK_DATA, client_process_output(&client));
    /* PUBLISH (QoS 0) packet should have been discarded */
    ck_assert_int_eq(TEST_CONNECT, test_socket_shift());
    ck_assert_int_eq(-1, test_socket_shift());
}
END_TEST

START_TEST(should_reset_decoder_on_reconnect)
{
    lmqtt_client_t client;

    do_init_connect_connack_process(&client, 5, 3);
    ck_assert_int_eq(1, do_publish(&client, 1));
    ck_assert_int_eq(LMQTT_IO_STATUS_BLOCK_DATA, client_process_output(&client));

    test_socket_append(TEST_PUBACK);

    /* Close remote side of connection after reading partial buffer */
    test_socket.read_buf.available_len -= 1;
    test_socket.read_buf.len = test_socket.read_buf.available_len;
    test_socket.test_pos_read -= 1;
    ck_assert_int_eq(LMQTT_IO_STATUS_READY, client_process_input(&client));

    memset(&connect, 0, sizeof(connect));
    connect.client_id.buf = "test";
    connect.client_id.len = 4;
    ck_assert_int_eq(1, lmqtt_client_connect(&client, &connect));
    ck_assert_int_eq(LMQTT_IO_STATUS_BLOCK_DATA, client_process_output(&client));

    /* should discard partial buffer */
    test_socket.read_buf.len = sizeof(test_socket.read_buf.buf);
    test_socket_append(TEST_CONNACK_SUCCESS);
    ck_assert_int_eq(LMQTT_IO_STATUS_BLOCK_CONN, client_process_input(&client));
}
END_TEST

START_TEST(should_finalize_client)
{
    lmqtt_client_t client;
    lmqtt_subscribe_t subscribe;
    lmqtt_subscribe_t unsubscribe;
    lmqtt_publish_t publish;
    lmqtt_subscription_t subscription;
    test_cb_result_t cb_result_1 = { 0, -1 };
    test_cb_result_t cb_result_2 = { 0, -1 };
    test_cb_result_t cb_result_3 = { 0, -1 };

    ck_assert_int_eq(1, do_init_connect_connack_process(&client, 5, 3));

    lmqtt_client_set_on_subscribe(&client, on_subscribe, &cb_result_1);
    lmqtt_client_set_on_unsubscribe(&client, on_unsubscribe, &cb_result_2);
    lmqtt_client_set_on_publish(&client, on_publish, &cb_result_3);

    memset(&subscribe, 0, sizeof(subscribe));
    memset(&unsubscribe, 0, sizeof(unsubscribe));
    memset(&subscription, 0, sizeof(subscription));
    subscribe.count = 1;
    subscribe.subscriptions = &subscription;
    unsubscribe.count = 1;
    unsubscribe.subscriptions = &subscription;
    subscription.qos = 0;
    subscription.topic.buf = "test";
    subscription.topic.len = strlen(subscription.topic.buf);
    memset(&publish, 0, sizeof(publish));
    publish.topic.buf = "topic";
    publish.topic.len = strlen(publish.topic.buf);

    ck_assert_int_eq(1, lmqtt_client_subscribe(&client, &subscribe));
    ck_assert_int_eq(1, lmqtt_client_unsubscribe(&client, &unsubscribe));
    ck_assert_int_eq(LMQTT_IO_STATUS_BLOCK_DATA, client_process_output(&client));
    /* publish after process_output; otherwise qos 0 packet will be dropped from
       queue immediattely after being sent */
    ck_assert_int_eq(1, lmqtt_client_publish(&client, &publish));

    lmqtt_client_finalize(&client);
    ck_assert_ptr_eq(&subscribe, cb_result_1.data);
    ck_assert_int_eq(0, cb_result_1.succeeded);
    ck_assert_ptr_eq(&unsubscribe, cb_result_2.data);
    ck_assert_int_eq(0, cb_result_2.succeeded);
    ck_assert_ptr_eq(&publish, cb_result_3.data);
    ck_assert_int_eq(0, cb_result_3.succeeded);
}
END_TEST

START_TEST(should_finalize_client_before_connack)
{
    lmqtt_client_t client;
    test_cb_result_t cb_result = { 0, -1 };

    do_init(&client, 5);

    lmqtt_client_set_on_connect(&client, &on_connect, &cb_result);
    ck_assert(do_connect_process(&client, 3));

    lmqtt_client_finalize(&client);
    ck_assert_ptr_eq(&connect, cb_result.data);
    ck_assert_int_eq(0, cb_result.succeeded);
}
END_TEST

START_TEST(should_finalize_client_after_partial_decode)
{
    lmqtt_client_t client;
    test_cb_result_t cb_result = { 0, -1 };

    do_init(&client, 5);

    lmqtt_client_set_on_connect(&client, &on_connect, &cb_result);
    ck_assert(do_connect_process(&client, 3));

    test_socket_append(TEST_CONNACK_FAILURE);

    test_socket.read_buf.available_len -= 1;
    test_socket.read_buf.len = test_socket.read_buf.available_len;
    test_socket.test_pos_read -= 1;
    ck_assert_int_eq(LMQTT_IO_STATUS_READY, client_process_input(&client));

    lmqtt_client_finalize(&client);
    ck_assert_ptr_eq(&connect, cb_result.data);
    ck_assert_int_eq(0, cb_result.succeeded);
}
END_TEST

START_TEST(should_not_reconnect_after_finalize)
{
    lmqtt_client_t client;

    ck_assert_int_eq(1, do_init_connect_connack_process(&client, 5, 3));

    lmqtt_client_finalize(&client);

    memset(&connect, 0, sizeof(connect));
    connect.clean_session = 1;

    ck_assert_int_eq(0, lmqtt_client_connect(&client, &connect));
    ck_assert_int_eq(LMQTT_IO_STATUS_ERROR, client_process_output(&client));
}
END_TEST

START_TEST(should_wait_connack_to_resend_packets_from_previous_connection)
{
    lmqtt_client_t client;

    do_init_connect_connack_process(&client, 5, 3);
    ck_assert(do_publish(&client, 1));
    ck_assert_int_eq(LMQTT_IO_STATUS_BLOCK_DATA, client_process_output(&client));
    ck_assert_int_eq(TEST_PUBLISH, test_socket_shift());

    ck_assert(close_read_buf(&client));
    ck_assert(do_connect_process(&client, 5));
    ck_assert_int_eq(-1, test_socket_shift());

    test_socket_append(TEST_CONNACK_SUCCESS);
    ck_assert_int_eq(LMQTT_IO_STATUS_BLOCK_CONN, client_process_input(&client));
    ck_assert_int_eq(LMQTT_IO_STATUS_BLOCK_DATA, client_process_output(&client));
    ck_assert_int_eq(TEST_PUBLISH, test_socket_shift());
    ck_assert_int_eq(-1, test_socket_shift());
}
END_TEST

START_TEST(should_wait_connack_to_send_unsent_packets_from_previous_connection)
{
    lmqtt_client_t client;

    do_init_connect_connack_process(&client, 5, 3);
    ck_assert(do_publish(&client, 1));
    ck_assert(close_read_buf(&client));
    ck_assert(do_connect_process(&client, 5));
    ck_assert_int_eq(-1, test_socket_shift());

    test_socket_append(TEST_CONNACK_SUCCESS);
    ck_assert_int_eq(LMQTT_IO_STATUS_BLOCK_CONN, client_process_input(&client));
    ck_assert_int_eq(LMQTT_IO_STATUS_BLOCK_DATA, client_process_output(&client));
    ck_assert_int_eq(TEST_PUBLISH, test_socket_shift());
    ck_assert_int_eq(-1, test_socket_shift());
}
END_TEST

START_TEST(should_not_resend_connect_from_previous_connection)
{
    lmqtt_client_t client;
    test_cb_result_t cb_result = { 0, -1 };
    lmqtt_connect_t connect_2;

    do_init(&client, 3);

    memset(&connect_2, 0, sizeof(connect_2));
    connect_2.keep_alive = 5;
    connect_2.clean_session = 1;

    lmqtt_client_set_on_connect(&client, &on_connect, &cb_result);
    lmqtt_client_connect(&client, &connect_2);

    ck_assert(close_read_buf(&client));
    ck_assert(do_connect_process(&client, 5));

    ck_assert_int_eq(-1, test_socket_shift());
    ck_assert_ptr_eq(&connect_2, cb_result.data);
    ck_assert_int_eq(0, cb_result.succeeded);
}
END_TEST

START_TCASE("Client commands")
{
    ADD_TEST(should_initialize_client);
    ADD_TEST(should_prepare_connect_after_initialize);
    ADD_TEST(should_not_prepare_connect_twice);
    ADD_TEST(should_not_prepare_invalid_connect);
    ADD_TEST(should_receive_connack_after_connect);
    ADD_TEST(should_call_connect_callback_on_connect_failure);
    ADD_TEST(should_not_receive_connack_before_connect);

    ADD_TEST(should_subscribe);
    ADD_TEST(should_unsubscribe);
    ADD_TEST(should_assign_packet_ids_to_subscribe);
    ADD_TEST(should_not_subscribe_with_invalid_packet);
    ADD_TEST(should_not_subscribe_with_full_store);

    ADD_TEST(should_publish_with_qos_0);
    ADD_TEST(should_publish_with_qos_1);
    ADD_TEST(should_publish_with_qos_2);
    ADD_TEST(should_not_publish_invalid_packet);

    ADD_TEST(should_send_pingreq_after_timeout);
    ADD_TEST(should_not_send_pingreq_before_timeout);
    ADD_TEST(should_not_send_pingreq_with_response_pending);
    ADD_TEST(should_send_pingreq_after_pingresp);
    ADD_TEST(should_fail_client_after_pingreq_timeout);
    ADD_TEST(should_disconnect);

    ADD_TEST(should_not_send_pingreq_after_disconnect);
    ADD_TEST(should_not_disconnect_before_connack);
    ADD_TEST(should_not_send_pingreq_after_failure);
    ADD_TEST(should_not_send_pingreq_before_connack);
    ADD_TEST(should_fail_client_after_connection_timeout);
    ADD_TEST(should_not_fail_client_before_connection_timeout);
    ADD_TEST(should_not_send_pingreq_with_zeroed_keep_alive);

    ADD_TEST(should_clean_pingreq_and_disconnect_packets_after_close);
    ADD_TEST(should_close_encoder_after_socket_close);
    ADD_TEST(should_close_encoder_after_connect_failure);
    ADD_TEST(should_reconnect_after_close);
    ADD_TEST(should_reconnect_after_disconnect);
    ADD_TEST(should_reset_output_buffer_on_reconnect);
    ADD_TEST(should_reset_decoder_on_reconnect);
    ADD_TEST(should_finalize_client);
    ADD_TEST(should_finalize_client_before_connack);
    ADD_TEST(should_finalize_client_after_partial_decode);
    ADD_TEST(should_not_reconnect_after_finalize);

    ADD_TEST(should_wait_connack_to_resend_packets_from_previous_connection);
    ADD_TEST(should_wait_connack_to_send_unsent_packets_from_previous_connection);
    ADD_TEST(should_not_resend_connect_from_previous_connection);
}
END_TCASE