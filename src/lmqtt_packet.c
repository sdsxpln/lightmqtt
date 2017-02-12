#include <lightmqtt/packet.h>
#include <string.h>
#include <assert.h>

#define LMQTT_TYPE_MIN 1
#define LMQTT_TYPE_CONNECT 1
#define LMQTT_TYPE_CONNACK 2
#define LMQTT_TYPE_PUBLISH 3
#define LMQTT_TYPE_PUBACK 4
#define LMQTT_TYPE_PUBREC 5
#define LMQTT_TYPE_PUBREL 6
#define LMQTT_TYPE_PUBCOMP 7
#define LMQTT_TYPE_SUBSCRIBE 8
#define LMQTT_TYPE_SUBACK 9
#define LMQTT_TYPE_UNSUBSCRIBE 10
#define LMQTT_TYPE_UNSUBACK 11
#define LMQTT_TYPE_PINGREQ 12
#define LMQTT_TYPE_PINGRESP 13
#define LMQTT_TYPE_DISCONNECT 14
#define LMQTT_TYPE_MAX 14

#define LMQTT_FLAG_CLEAN_SESSION 0x02
#define LMQTT_FLAG_WILL_FLAG 0x04
#define LMQTT_FLAG_WILL_RETAIN 0x20
#define LMQTT_FLAG_PASSWORD_FLAG 0x40
#define LMQTT_FLAG_USER_NAME_FLAG 0x80
#define LMQTT_OFFSET_FLAG_QOS 3

#define LMQTT_CONNACK_RC_ACCEPTED 0
#define LMQTT_CONNACK_RC_UNACCEPTABLE_PROTOCOL_VERSION 1
#define LMQTT_CONNACK_RC_IDENTIFIER_REJECTED 2
#define LMQTT_CONNACK_RC_SERVER_UNAVAILABLE 3
#define LMQTT_CONNACK_RC_BAD_USER_NAME_OR_PASSWORD 4
#define LMQTT_CONNACK_RC_NOT_AUTHORIZED 5
#define LMQTT_CONNACK_RC_MAX 5

#define LMQTT_STRING_LEN_SIZE 2

#define STRING_LEN_BYTE(val, num) (((val) >> ((num) * 8)) & 0xff)

/* caller must guarantee buf is at least 4-bytes long! */
static int encode_remaining_length(int len, u8 *buf, int *bytes_written)
{
    int pos;

    if (len < 0 || len > 0x0fffffff)
        return LMQTT_ENCODE_ERROR;

    pos = 0;
    do {
        u8 b = len % 128;
        len /= 128;
        buf[pos++] = len > 0 ? b | 0x80 : b;
    } while (len > 0);

    *bytes_written = pos;
    return LMQTT_ENCODE_FINISHED;
}

static int encode_string(lmqtt_string_t *str, int encode_if_empty, int offset,
    u8 *buf, int buf_len, int *bytes_written)
{
    int len = str->len;
    int result;
    int pos = 0;
    int offset_str;
    int i;

    if (len == 0 && !encode_if_empty) {
        *bytes_written = 0;
        return LMQTT_ENCODE_FINISHED;
    }

    assert(offset < buf_len && buf_len > 0);

    for (i = 0; i < LMQTT_STRING_LEN_SIZE; i++) {
        if (offset <= i) {
            buf[pos++] = STRING_LEN_BYTE(len, LMQTT_STRING_LEN_SIZE - i - 1);
            if (pos >= buf_len) {
                *bytes_written = pos;
                return pos >= LMQTT_STRING_LEN_SIZE && len == 0 ?
                    LMQTT_ENCODE_FINISHED : LMQTT_ENCODE_AGAIN;
            }
        }
    }

    offset_str = offset <= LMQTT_STRING_LEN_SIZE ? 0 :
        offset - LMQTT_STRING_LEN_SIZE;
    len -= offset_str;

    if (len > buf_len - pos) {
        len = buf_len - pos;
        result = LMQTT_ENCODE_AGAIN;
    } else {
        result = LMQTT_ENCODE_FINISHED;
    }

    memcpy(buf + pos, str->buf + offset_str, len);
    *bytes_written = pos + len;
    return result;
}

static int calc_connect_payload_field_length(lmqtt_string_t *str)
{
    return str->len > 0 ? LMQTT_STRING_LEN_SIZE + str->len : 0;
}

static int calc_connect_remaining_legth(lmqtt_connect_t *connect)
{
    return LMQTT_CONNECT_HEADER_SIZE +
        /* client_id is always present in payload */
        LMQTT_STRING_LEN_SIZE + connect->client_id.len +
        calc_connect_payload_field_length(&connect->will_topic) +
        calc_connect_payload_field_length(&connect->will_message) +
        calc_connect_payload_field_length(&connect->user_name) +
        calc_connect_payload_field_length(&connect->password);
}

static int validate_payload_field_length(lmqtt_string_t *str)
{
    return str->len >= 0 && str->len <= 0xffff;
}

static int validate_connect(lmqtt_connect_t *connect)
{
    if (!validate_payload_field_length(&connect->client_id) ||
            !validate_payload_field_length(&connect->will_topic) ||
            !validate_payload_field_length(&connect->will_message) ||
            !validate_payload_field_length(&connect->user_name) ||
            !validate_payload_field_length(&connect->password))
        return 0;

    if (connect->will_topic.len == 0 ^ connect->will_message.len == 0)
        return 0;

    if (connect->will_topic.len == 0 && connect->will_retain)
        return 0;

    if (connect->client_id.len == 0 && !connect->clean_session)
        return 0;

    if (connect->user_name.len == 0 && connect->password.len != 0)
        return 0;

    if (connect->qos < 0 || connect->qos > 2)
        return 0;

    return 1;
}

static int encode_from_temp_buffer(int (*func)(lmqtt_connect_t *connect),
    lmqtt_connect_t *connect, int offset, u8 *buf, int buf_len, int *bytes_written)
{
    int cnt;
    int result;

    assert(buf_len >= 0);
    assert(offset == 0 || connect->internal.buf_len > 0 &&
        offset < connect->internal.buf_len);

    if (offset == 0 && func(connect) != LMQTT_ENCODE_FINISHED)
        return LMQTT_ENCODE_ERROR;

    cnt = connect->internal.buf_len - offset;
    result = LMQTT_ENCODE_FINISHED;

    if (cnt > buf_len) {
        cnt = buf_len;
        result = LMQTT_ENCODE_AGAIN;
    }

    memcpy(buf, &connect->internal.buf[offset], cnt);
    *bytes_written = cnt;

    if (result == LMQTT_ENCODE_FINISHED) {
        connect->internal.buf_len = 0;
        memset(connect->internal.buf, 0, sizeof(connect->internal.buf));
    }

    return result;
}

static int encode_connect_fixed_header_builder(lmqtt_connect_t *connect)
{
    int remain_len_size;
    int res;

    assert(sizeof(connect->internal.buf) >= LMQTT_FIXED_HEADER_MAX_SIZE);

    res = encode_remaining_length(calc_connect_remaining_legth(connect),
        connect->internal.buf + 1, &remain_len_size);
    if (res != LMQTT_ENCODE_FINISHED)
        return LMQTT_ENCODE_ERROR;

    connect->internal.buf[0] = LMQTT_TYPE_CONNECT << 4;
    connect->internal.buf_len = 1 + remain_len_size;
    return LMQTT_ENCODE_FINISHED;
}

static int encode_connect_fixed_header(lmqtt_connect_t *connect, int offset,
    u8 *buf, int buf_len, int *bytes_written)
{
    return encode_from_temp_buffer(encode_connect_fixed_header_builder,
        connect, offset, buf, buf_len, bytes_written);
}

static int encode_connect_variable_header_builder(lmqtt_connect_t *connect)
{
    u8 flags;

    assert(sizeof(connect->internal.buf) >= LMQTT_CONNECT_HEADER_SIZE);

    memcpy(connect->internal.buf, "\x00\x04MQTT\x04", 7);

    flags = connect->qos << LMQTT_OFFSET_FLAG_QOS;
    if (connect->clean_session)
        flags |= LMQTT_FLAG_CLEAN_SESSION;
    if (connect->will_retain)
        flags |= LMQTT_FLAG_WILL_RETAIN;
    if (connect->will_topic.len > 0)
        flags |= LMQTT_FLAG_WILL_FLAG;
    if (connect->user_name.len > 0)
        flags |= LMQTT_FLAG_USER_NAME_FLAG;
    if (connect->password.len > 0)
        flags |= LMQTT_FLAG_PASSWORD_FLAG;
    connect->internal.buf[7] = flags;

    connect->internal.buf[8] = STRING_LEN_BYTE(connect->keep_alive, 1);
    connect->internal.buf[9] = STRING_LEN_BYTE(connect->keep_alive, 0);
    connect->internal.buf_len = 10;
    return LMQTT_ENCODE_FINISHED;
}

static int encode_connect_variable_header(lmqtt_connect_t *connect, int offset,
    u8 *buf, int buf_len, int *bytes_written)
{
    return encode_from_temp_buffer(encode_connect_variable_header_builder,
        connect, offset, buf, buf_len, bytes_written);
}

static int encode_connect_payload_client_id(lmqtt_connect_t *connect, int offset,
    u8 *buf, int buf_len, int *bytes_written)
{
    return encode_string(&connect->client_id, 1, offset, buf, buf_len,
        bytes_written);
}

static int encode_connect_payload_will_topic(lmqtt_connect_t *connect, int offset,
    u8 *buf, int buf_len, int *bytes_written)
{
    return encode_string(&connect->will_topic, 0, offset, buf, buf_len,
        bytes_written);
}

static int encode_connect_payload_will_message(lmqtt_connect_t *connect, int offset,
    u8 *buf, int buf_len, int *bytes_written)
{
    return encode_string(&connect->will_message, 0, offset, buf, buf_len,
        bytes_written);
}

static int encode_connect_payload_user_name(lmqtt_connect_t *connect, int offset,
    u8 *buf, int buf_len, int *bytes_written)
{
    return encode_string(&connect->user_name, 0, offset, buf, buf_len,
        bytes_written);
}

static int encode_connect_payload_password(lmqtt_connect_t *connect, int offset,
    u8 *buf, int buf_len, int *bytes_written)
{
    return encode_string(&connect->password, 0, offset, buf, buf_len,
        bytes_written);
}

static int fixed_header_decode(lmqtt_fixed_header_t *header, u8 b)
{
    int result = LMQTT_DECODE_ERROR;

    if (header->internal.failed)
        return LMQTT_DECODE_ERROR;

    if (header->internal.bytes_read == 0) {
        int type = b >> 4;
        int flags = b & 0x0f;
        int bad_flags;

        switch (type) {
            case LMQTT_TYPE_PUBREL:
            case LMQTT_TYPE_SUBSCRIBE:
            case LMQTT_TYPE_UNSUBSCRIBE:
                bad_flags = flags != 2;
                break;
            case LMQTT_TYPE_PUBLISH:
                bad_flags = (flags & 6) == 6;
                break;
            default:
                bad_flags = flags != 0;
        }

        if (type < LMQTT_TYPE_MIN || type > LMQTT_TYPE_MAX || bad_flags) {
            result = LMQTT_DECODE_ERROR;
        } else {
            header->type = type;
            header->internal.remain_len_multiplier = 1;
            header->internal.remain_len_accumulator = 0;
            header->internal.remain_len_finished = 0;
            if (type == LMQTT_TYPE_PUBLISH) {
                header->dup = (flags & 8) >> 3;
                header->qos = (flags & 6) >> 1;
                header->retain = flags & 1;
            } else {
                header->dup = 0;
                header->qos = 0;
                header->retain = 0;
            }
            result = LMQTT_DECODE_AGAIN;
        }
    } else {
        if (header->internal.remain_len_multiplier > 128 * 128 && (b & 128) != 0 ||
                header->internal.remain_len_multiplier > 1 && b == 0 ||
                header->internal.remain_len_finished) {
            result = LMQTT_DECODE_ERROR;
        } else {
            header->internal.remain_len_accumulator += (b & 127) *
                header->internal.remain_len_multiplier;
            header->internal.remain_len_multiplier *= 128;

            if (b & 128) {
                result = LMQTT_DECODE_AGAIN;
            } else {
                header->remaining_length =
                    header->internal.remain_len_accumulator;
                header->internal.remain_len_finished = 1;
                result = LMQTT_DECODE_FINISHED;
            }
        }
    }

    if (result == LMQTT_DECODE_ERROR)
        header->internal.failed = 1;
    else
        header->internal.bytes_read += 1;
    return result;
}

static int connack_decode(lmqtt_connack_t *connack, u8 b)
{
    int result = LMQTT_DECODE_ERROR;

    if (connack->internal.failed)
        return LMQTT_DECODE_ERROR;

    switch (connack->internal.bytes_read) {
    case 0:
        if (b & ~1) {
            result = LMQTT_DECODE_ERROR;
        } else {
            connack->session_present = b != 0;
            result = LMQTT_DECODE_AGAIN;
        }
        break;
    case 1:
        if (b > LMQTT_CONNACK_RC_MAX) {
            result = LMQTT_DECODE_ERROR;
        } else {
            connack->return_code = b;
            result = LMQTT_DECODE_FINISHED;
        }
        break;
    }

    if (result == LMQTT_DECODE_ERROR)
        connack->internal.failed = 1;
    else
        connack->internal.bytes_read += 1;
    return result;
}

/*
 * TODO: maybe encode_tx_buffer(), like encode_tx_buffer(), should return
 * LMQTT_ENCODE_AGAIN only when building it would block?
 */
int encode_tx_buffer(lmqtt_tx_buffer_state_t *state, u8 *buf, int buf_len,
    int *bytes_written)
{
    int offset = 0;
    *bytes_written = 0;

    while (1) {
        int result;
        int cur_bytes;
        lmqtt_encode_t recipe = state->recipe[state->internal.recipe_pos];

        if (!recipe)
            break;

        result = recipe(state->data, state->internal.recipe_offset,
            buf + offset, buf_len - offset, &cur_bytes);
        if (result == LMQTT_ENCODE_AGAIN)
            state->internal.recipe_offset += cur_bytes;
        if (result == LMQTT_ENCODE_AGAIN || result == LMQTT_ENCODE_FINISHED)
            *bytes_written += cur_bytes;
        if (result != LMQTT_ENCODE_FINISHED)
            return result;

        offset += cur_bytes;
        state->internal.recipe_pos += 1;
        state->internal.recipe_offset = 0;
    }

    return LMQTT_ENCODE_FINISHED;
}

/*
 * Return: 1 on success, 0 on failure
 */
static int rx_buffer_state_call_callback(lmqtt_rx_buffer_state_t *state)
{
    int result = 1;

    switch (state->internal.header.type) {
        case LMQTT_TYPE_CONNACK:
            state->callbacks->on_connack(state->callbacks_data,
                &state->internal.payload.connack);
            break;
        case LMQTT_TYPE_PINGRESP:
            state->callbacks->on_pingresp(state->callbacks_data);
            break;
        default:
            result = 0;
    }

    state->internal.header_finished = 0;
    memset(&state->internal.header, 0, sizeof(state->internal.header));
    memset(&state->internal.payload, 0, sizeof(state->internal.payload));

    return result;
}

static int rx_buffer_state_decode_type(lmqtt_rx_buffer_state_t *state, u8 b)
{
    switch (state->internal.header.type) {
        case LMQTT_TYPE_CONNACK:
            return connack_decode(&state->internal.payload.connack, b);
        case LMQTT_TYPE_PUBLISH:
        case LMQTT_TYPE_PUBACK:
        case LMQTT_TYPE_PUBREC:
        case LMQTT_TYPE_PUBREL:
        case LMQTT_TYPE_PUBCOMP:
        case LMQTT_TYPE_SUBACK:
        case LMQTT_TYPE_UNSUBACK:
        default:
            assert(0);
    }
}

static int rx_buffer_state_fail(lmqtt_rx_buffer_state_t *state)
{
    state->internal.failed = 1;
    return LMQTT_DECODE_ERROR;
}

static int rx_buffer_state_is_acceptable_response_packet(
    lmqtt_rx_buffer_state_t *state)
{
    switch (state->internal.header.type) {
        case LMQTT_TYPE_CONNACK:
        case LMQTT_TYPE_PUBLISH:
        case LMQTT_TYPE_PUBACK:
        case LMQTT_TYPE_PUBREC:
        case LMQTT_TYPE_PUBREL:
        case LMQTT_TYPE_PUBCOMP:
        case LMQTT_TYPE_SUBACK:
        case LMQTT_TYPE_UNSUBACK:
        case LMQTT_TYPE_PINGRESP:
            return 1;
        default:
            return 0;
    }
}

static int rx_buffer_state_is_zero_length_packet(lmqtt_rx_buffer_state_t *state)
{
    switch (state->internal.header.type) {
        case LMQTT_TYPE_PINGREQ:
        case LMQTT_TYPE_PINGRESP:
        case LMQTT_TYPE_DISCONNECT:
            return 1;
        default:
            return 0;
    }
}

/*
 * TODO: decode_rx_buffer() should be able to handle cases where the buffer
 * cannot be completely read (for example, if a callback which is being invoked
 * to write the incoming data to a file would block) and return
 * LMQTT_DECODE_AGAIN. Otherwise it should return LMQTT_DECODE_FINISHED, even if
 * the incoming packet is not yet complete. (That may look confusing. Should we
 * have different return codes for decode_rx_buffer() and the other decoding
 * functions?)
 */
int decode_rx_buffer(lmqtt_rx_buffer_state_t *state, u8 *buf, int buf_len,
    int *bytes_read)
{
    int i;

    *bytes_read = 0;

    if (state->internal.failed)
        return LMQTT_DECODE_ERROR;

    for (i = 0; i < buf_len; i++) {
        *bytes_read += 1;

        if (!state->internal.header_finished) {
            int actual_is_zero;
            int expected_is_zero;
            int res = fixed_header_decode(&state->internal.header, buf[i]);

            if (res == LMQTT_DECODE_ERROR)
                return rx_buffer_state_fail(state);
            if (res != LMQTT_DECODE_FINISHED)
                continue;

            state->internal.header_finished = 1;

            if (!rx_buffer_state_is_acceptable_response_packet(state))
                return rx_buffer_state_fail(state);

            actual_is_zero = state->internal.header.remaining_length == 0;
            expected_is_zero = rx_buffer_state_is_zero_length_packet(state);

            if (actual_is_zero != expected_is_zero)
                return rx_buffer_state_fail(state);
            if (actual_is_zero && expected_is_zero)
                rx_buffer_state_call_callback(state);
        } else {
            int res = rx_buffer_state_decode_type(state, buf[i]);

            if (res == LMQTT_DECODE_ERROR)
                return rx_buffer_state_fail(state);
            if (res != LMQTT_DECODE_FINISHED)
                continue;

            rx_buffer_state_call_callback(state);
        }
    }

    return LMQTT_DECODE_FINISHED;
}