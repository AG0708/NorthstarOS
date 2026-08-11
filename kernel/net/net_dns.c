#include <northstar/net_dns.h>

#define DNS_HEADER_LENGTH 12u
#define DNS_TYPE_A 1u
#define DNS_TYPE_CNAME 5u
#define DNS_CLASS_IN 1u
#define DNS_FLAG_QUERY_RECURSION UINT16_C(0x0100)
#define DNS_FLAG_RESPONSE UINT16_C(0x8000)
#define DNS_FLAG_TRUNCATED UINT16_C(0x0200)
#define DNS_OPCODE_MASK UINT16_C(0x7800)
#define DNS_RCODE_MASK UINT16_C(0x000f)
#define DNS_RCODE_NOERROR 0u
#define DNS_RCODE_NXDOMAIN 3u
#define DNS_RETRY_BASE_MS UINT64_C(1000)
#define DNS_TRANSACTION_TIMEOUT_MS UINT64_C(7000)
#define DNS_MAX_RR_COUNT 32u
#define DNS_MAX_CNAME_RECORDS 8u
#define DNS_MAX_A_RECORDS 8u
#define DNS_MAX_POINTER_JUMPS 32u

struct dns_cname_record {
    char owner[NET_DNS_MAX_NAME_LENGTH + 1u];
    char target[NET_DNS_MAX_NAME_LENGTH + 1u];
    uint32_t ttl_seconds;
    bool traversed;
};

struct dns_a_record {
    char owner[NET_DNS_MAX_NAME_LENGTH + 1u];
    uint32_t address;
    uint32_t ttl_seconds;
};

static void bytes_zero(void *memory, size_t length)
{
    uint8_t *bytes = (uint8_t *)memory;
    size_t index;

    for (index = 0u; index < length; ++index) {
        bytes[index] = 0u;
    }
}

static size_t string_length_bounded(const char *text, size_t maximum)
{
    size_t length = 0u;

    if (text == NULL) {
        return maximum + 1u;
    }
    while (length <= maximum && text[length] != '\0') {
        ++length;
    }
    return length;
}

static bool strings_equal(const char *left, const char *right)
{
    size_t index = 0u;

    while (left[index] != '\0' && right[index] != '\0') {
        if (left[index] != right[index]) {
            return false;
        }
        ++index;
    }
    return left[index] == right[index];
}

static void string_copy(char *destination, const char *source, size_t capacity)
{
    size_t index = 0u;

    if (capacity == 0u) {
        return;
    }
    while (index + 1u < capacity && source[index] != '\0') {
        destination[index] = source[index];
        ++index;
    }
    destination[index] = '\0';
}

static char ascii_lower(char character)
{
    if (character >= 'A' && character <= 'Z') {
        return (char)(character + ('a' - 'A'));
    }
    return character;
}

static uint16_t read_be16(const uint8_t *bytes)
{
    return (uint16_t)(((uint16_t)bytes[0] << 8u) | (uint16_t)bytes[1]);
}

static uint32_t read_be32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24u) | ((uint32_t)bytes[1] << 16u) |
           ((uint32_t)bytes[2] << 8u) | (uint32_t)bytes[3];
}

static void write_be16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)(value >> 8u);
    bytes[1] = (uint8_t)value;
}

static uint64_t add_saturated(uint64_t value, uint64_t amount)
{
    if (amount > UINT64_MAX - value) {
        return UINT64_MAX;
    }
    return value + amount;
}

static uint64_t ttl_deadline(uint64_t now_ms, uint32_t ttl_seconds)
{
    return add_saturated(now_ms, (uint64_t)ttl_seconds * UINT64_C(1000));
}

static bool canonicalize_name(const char *name,
                              char canonical[NET_DNS_MAX_NAME_LENGTH + 1u])
{
    size_t input_length =
        string_length_bounded(name, NET_DNS_MAX_NAME_LENGTH + 1u);
    size_t canonical_length;
    size_t label_length = 0u;
    size_t index;

    if (input_length == 0u || input_length > NET_DNS_MAX_NAME_LENGTH + 1u) {
        return false;
    }
    canonical_length = input_length;
    if (name[canonical_length - 1u] == '.') {
        --canonical_length;
    }
    if (canonical_length == 0u ||
        canonical_length > NET_DNS_MAX_NAME_LENGTH) {
        return false;
    }
    for (index = 0u; index < canonical_length; ++index) {
        unsigned char character = (unsigned char)name[index];

        if (character == (unsigned char)'.') {
            if (label_length == 0u || label_length > 63u) {
                return false;
            }
            label_length = 0u;
            canonical[index] = '.';
            continue;
        }
        if (character < 0x21u || character > 0x7eu || character == '\\') {
            return false;
        }
        ++label_length;
        if (label_length > 63u) {
            return false;
        }
        canonical[index] = ascii_lower((char)character);
    }
    if (label_length == 0u || label_length > 63u) {
        return false;
    }
    canonical[canonical_length] = '\0';
    return true;
}

bool net_dns_encode_name(const char *name,
                         uint8_t *encoded,
                         size_t encoded_capacity,
                         size_t *encoded_length)
{
    char canonical[NET_DNS_MAX_NAME_LENGTH + 1u];
    size_t read_offset = 0u;
    size_t write_offset = 0u;

    if (encoded == NULL || encoded_length == NULL ||
        !canonicalize_name(name, canonical)) {
        return false;
    }
    while (canonical[read_offset] != '\0') {
        size_t label_start = read_offset;
        size_t label_length;
        size_t index;

        while (canonical[read_offset] != '\0' &&
               canonical[read_offset] != '.') {
            ++read_offset;
        }
        label_length = read_offset - label_start;
        if (write_offset >= encoded_capacity ||
            label_length > encoded_capacity - write_offset - 1u) {
            return false;
        }
        encoded[write_offset++] = (uint8_t)label_length;
        for (index = 0u; index < label_length; ++index) {
            encoded[write_offset++] =
                (uint8_t)canonical[label_start + index];
        }
        if (canonical[read_offset] == '.') {
            ++read_offset;
        }
    }
    if (write_offset >= encoded_capacity) {
        return false;
    }
    encoded[write_offset++] = 0u;
    *encoded_length = write_offset;
    return true;
}

static bool decode_name(const uint8_t *packet,
                        size_t packet_length,
                        size_t *offset,
                        char output[NET_DNS_MAX_NAME_LENGTH + 1u])
{
    size_t position;
    size_t output_length = 0u;
    size_t pointer_return = 0u;
    uint16_t visited[DNS_MAX_POINTER_JUMPS];
    size_t visited_count = 0u;
    size_t labels = 0u;
    bool jumped = false;

    if (packet == NULL || offset == NULL || output == NULL ||
        *offset >= packet_length) {
        return false;
    }
    position = *offset;
    for (;;) {
        uint8_t length;
        size_t index;

        if (position >= packet_length) {
            return false;
        }
        length = packet[position];
        if ((length & 0xc0u) == 0xc0u) {
            uint16_t target;

            if (position + 1u >= packet_length ||
                visited_count >= DNS_MAX_POINTER_JUMPS) {
                return false;
            }
            target = (uint16_t)(((uint16_t)(length & 0x3fu) << 8u) |
                                packet[position + 1u]);
            if ((size_t)target >= packet_length) {
                return false;
            }
            for (index = 0u; index < visited_count; ++index) {
                if (visited[index] == target) {
                    return false;
                }
            }
            visited[visited_count++] = target;
            if (!jumped) {
                pointer_return = position + 2u;
                jumped = true;
            }
            position = target;
            continue;
        }
        if ((length & 0xc0u) != 0u || length > 63u) {
            return false;
        }
        ++position;
        if (length == 0u) {
            *offset = jumped ? pointer_return : position;
            output[output_length] = '\0';
            return true;
        }
        if (++labels > 127u || (size_t)length > packet_length - position) {
            return false;
        }
        if (output_length != 0u) {
            if (output_length >= NET_DNS_MAX_NAME_LENGTH) {
                return false;
            }
            output[output_length++] = '.';
        }
        if ((size_t)length > NET_DNS_MAX_NAME_LENGTH - output_length) {
            return false;
        }
        for (index = 0u; index < (size_t)length; ++index) {
            unsigned char character = packet[position + index];

            if (character < 0x21u || character > 0x7eu ||
                character == (unsigned char)'.' || character == '\\') {
                return false;
            }
            output[output_length++] = ascii_lower((char)character);
        }
        position += length;
    }
}

static bool identifier_in_use(const struct net_dns_client *client,
                              uint16_t identifier)
{
    size_t index;

    for (index = 0u; index < NET_DNS_MAX_TRANSACTIONS; ++index) {
        if (client->transactions[index].in_use &&
            client->transactions[index].identifier == identifier) {
            return true;
        }
    }
    return false;
}

static uint16_t allocate_identifier(struct net_dns_client *client)
{
    uint32_t attempts;

    for (attempts = 0u; attempts <= UINT16_MAX; ++attempts) {
        uint16_t candidate = client->next_identifier++;

        if (!identifier_in_use(client, candidate)) {
            return candidate;
        }
    }
    return 0u;
}

static size_t build_query(const struct net_dns_transaction *transaction,
                          uint8_t packet[NET_DNS_MAX_PACKET_SIZE])
{
    size_t encoded_length;
    size_t offset = DNS_HEADER_LENGTH;

    bytes_zero(packet, NET_DNS_MAX_PACKET_SIZE);
    write_be16(&packet[0], transaction->identifier);
    write_be16(&packet[2], DNS_FLAG_QUERY_RECURSION);
    write_be16(&packet[4], 1u);
    if (!net_dns_encode_name(transaction->name, &packet[offset],
                             NET_DNS_MAX_PACKET_SIZE - offset,
                             &encoded_length)) {
        return 0u;
    }
    offset += encoded_length;
    if (NET_DNS_MAX_PACKET_SIZE - offset < 4u) {
        return 0u;
    }
    write_be16(&packet[offset], DNS_TYPE_A);
    write_be16(&packet[offset + 2u], DNS_CLASS_IN);
    return offset + 4u;
}

static bool send_query(struct net_dns_client *client,
                       const struct net_dns_transaction *transaction)
{
    uint8_t packet[NET_DNS_MAX_PACKET_SIZE];
    size_t packet_length = build_query(transaction, packet);

    return packet_length != 0u && client->send != NULL &&
           client->send(client->send_context, client->local_address,
                        client->server_address, transaction->source_port,
                        NET_DNS_PORT, packet, packet_length);
}

static struct net_dns_transaction *find_transaction(
    struct net_dns_client *client, uint16_t identifier, uint16_t source_port)
{
    size_t index;

    for (index = 0u; index < NET_DNS_MAX_TRANSACTIONS; ++index) {
        struct net_dns_transaction *transaction = &client->transactions[index];

        if (transaction->in_use && transaction->identifier == identifier &&
            transaction->source_port == source_port) {
            return transaction;
        }
    }
    return NULL;
}

static void initialize_result(struct net_dns_result *result,
                              enum net_dns_status status,
                              const char *query_name)
{
    bytes_zero(result, sizeof(*result));
    result->status = status;
    string_copy(result->query_name, query_name, sizeof(result->query_name));
    string_copy(result->canonical_name, query_name,
                sizeof(result->canonical_name));
}

static void deliver_result(struct net_dns_transaction *transaction,
                           const struct net_dns_result *result)
{
    net_dns_result_fn callback = transaction->callback;
    void *callback_context = transaction->callback_context;

    transaction->in_use = false;
    transaction->callback = NULL;
    transaction->callback_context = NULL;
    if (callback != NULL) {
        callback(callback_context, result);
    }
}

static struct net_dns_cache_entry *find_cache_entry(
    struct net_dns_client *client, const char *name, uint64_t now_ms)
{
    size_t index;

    for (index = 0u; index < NET_DNS_MAX_CACHE_ENTRIES; ++index) {
        struct net_dns_cache_entry *entry = &client->cache[index];

        if (entry->in_use && now_ms >= entry->expires_at_ms) {
            entry->in_use = false;
        }
        if (entry->in_use && strings_equal(entry->name, name)) {
            return entry;
        }
    }
    return NULL;
}

static void cache_result(struct net_dns_client *client,
                         const struct net_dns_result *result,
                         uint64_t now_ms)
{
    struct net_dns_cache_entry *selected = NULL;
    uint64_t oldest_sequence = UINT64_MAX;
    size_t index;

    if (result->status != NET_DNS_STATUS_OK || result->address_count == 0u ||
        result->ttl_seconds == 0u) {
        return;
    }
    for (index = 0u; index < NET_DNS_MAX_CACHE_ENTRIES; ++index) {
        struct net_dns_cache_entry *entry = &client->cache[index];

        if (entry->in_use && strings_equal(entry->name, result->query_name)) {
            selected = entry;
            break;
        }
        if (!entry->in_use || now_ms >= entry->expires_at_ms) {
            selected = entry;
            break;
        }
        if (entry->insertion_sequence < oldest_sequence) {
            oldest_sequence = entry->insertion_sequence;
            selected = entry;
        }
    }
    if (selected == NULL) {
        return;
    }
    bytes_zero(selected, sizeof(*selected));
    selected->in_use = true;
    string_copy(selected->name, result->query_name, sizeof(selected->name));
    string_copy(selected->canonical_name, result->canonical_name,
                sizeof(selected->canonical_name));
    selected->address_count = result->address_count;
    for (index = 0u; index < result->address_count; ++index) {
        selected->addresses[index] = result->addresses[index];
    }
    selected->ttl_seconds = result->ttl_seconds;
    selected->expires_at_ms =
        ttl_deadline(now_ms, result->ttl_seconds);
    selected->insertion_sequence = ++client->cache_sequence;
}

static enum net_dns_receive_result malformed_transaction(
    struct net_dns_transaction *transaction)
{
    struct net_dns_result result;

    initialize_result(&result, NET_DNS_STATUS_MALFORMED, transaction->name);
    deliver_result(transaction, &result);
    return NET_DNS_RX_MALFORMED;
}

void net_dns_init(struct net_dns_client *client,
                  uint32_t local_address,
                  uint32_t server_address,
                  uint16_t identifier_seed,
                  uint16_t source_port_base,
                  net_dns_send_fn send,
                  void *send_context)
{
    if (client == NULL) {
        return;
    }
    bytes_zero(client, sizeof(*client));
    client->local_address = local_address;
    client->server_address = server_address;
    client->next_identifier = identifier_seed;
    if (source_port_base < 1024u ||
        source_port_base > (uint16_t)(UINT16_MAX -
                                      (NET_DNS_MAX_TRANSACTIONS - 1u))) {
        client->source_port_base = UINT16_C(49152);
    } else {
        client->source_port_base = source_port_base;
    }
    client->send = send;
    client->send_context = send_context;
}

void net_dns_set_network(struct net_dns_client *client,
                         uint32_t local_address,
                         uint32_t server_address)
{
    if (client == NULL) {
        return;
    }
    client->local_address = local_address;
    client->server_address = server_address;
}

enum net_dns_submit_result
net_dns_resolve(struct net_dns_client *client,
                const char *name,
                uint64_t now_ms,
                net_dns_result_fn callback,
                void *callback_context)
{
    char canonical[NET_DNS_MAX_NAME_LENGTH + 1u];
    struct net_dns_cache_entry *cached;
    struct net_dns_transaction *transaction = NULL;
    size_t slot = 0u;
    size_t index;

    if (client == NULL || callback == NULL ||
        !canonicalize_name(name, canonical)) {
        return NET_DNS_SUBMIT_INVALID_NAME;
    }
    cached = find_cache_entry(client, canonical, now_ms);
    if (cached != NULL) {
        struct net_dns_result result;
        uint64_t remaining_ms = cached->expires_at_ms - now_ms;

        initialize_result(&result, NET_DNS_STATUS_OK, canonical);
        string_copy(result.canonical_name, cached->canonical_name,
                    sizeof(result.canonical_name));
        result.address_count = cached->address_count;
        for (index = 0u; index < cached->address_count; ++index) {
            result.addresses[index] = cached->addresses[index];
        }
        result.ttl_seconds = (uint32_t)(remaining_ms / UINT64_C(1000));
        if (result.ttl_seconds == 0u) {
            result.ttl_seconds = 1u;
        }
        result.from_cache = true;
        callback(callback_context, &result);
        return NET_DNS_SUBMIT_COMPLETED_FROM_CACHE;
    }

    for (slot = 0u; slot < NET_DNS_MAX_TRANSACTIONS; ++slot) {
        if (!client->transactions[slot].in_use) {
            transaction = &client->transactions[slot];
            break;
        }
    }
    if (transaction == NULL) {
        return NET_DNS_SUBMIT_NO_RESOURCES;
    }
    bytes_zero(transaction, sizeof(*transaction));
    transaction->in_use = true;
    transaction->identifier = allocate_identifier(client);
    transaction->source_port = (uint16_t)(client->source_port_base + slot);
    string_copy(transaction->name, canonical, sizeof(transaction->name));
    transaction->callback = callback;
    transaction->callback_context = callback_context;
    transaction->attempts = 1u;
    transaction->next_retry_at_ms =
        add_saturated(now_ms, DNS_RETRY_BASE_MS);
    transaction->deadline_at_ms =
        add_saturated(now_ms, DNS_TRANSACTION_TIMEOUT_MS);
    if (!send_query(client, transaction)) {
        struct net_dns_result result;

        initialize_result(&result, NET_DNS_STATUS_SEND_FAILED, canonical);
        deliver_result(transaction, &result);
        return NET_DNS_SUBMIT_SEND_FAILED;
    }
    return NET_DNS_SUBMIT_STARTED;
}

enum net_dns_receive_result
net_dns_receive(struct net_dns_client *client,
                uint32_t source_address,
                uint16_t destination_port,
                const uint8_t *payload,
                size_t payload_length,
                uint64_t now_ms)
{
    struct net_dns_transaction *transaction;
    struct dns_cname_record cnames[DNS_MAX_CNAME_RECORDS];
    struct dns_a_record addresses[DNS_MAX_A_RECORDS];
    size_t cname_count = 0u;
    size_t address_record_count = 0u;
    uint16_t flags;
    uint16_t question_count;
    uint16_t answer_count;
    uint16_t authority_count;
    uint16_t additional_count;
    uint32_t total_records;
    size_t offset = DNS_HEADER_LENGTH;
    char question_name[NET_DNS_MAX_NAME_LENGTH + 1u];
    uint16_t question_type;
    uint16_t question_class;
    uint32_t section;
    struct net_dns_result result;

    if (client == NULL || payload == NULL || payload_length < DNS_HEADER_LENGTH ||
        payload_length > NET_DNS_MAX_PACKET_SIZE) {
        return NET_DNS_RX_MALFORMED;
    }
    if (source_address != client->server_address) {
        return NET_DNS_RX_IGNORED;
    }
    transaction = find_transaction(client, read_be16(&payload[0]),
                                   destination_port);
    if (transaction == NULL) {
        return NET_DNS_RX_IGNORED;
    }
    flags = read_be16(&payload[2]);
    question_count = read_be16(&payload[4]);
    answer_count = read_be16(&payload[6]);
    authority_count = read_be16(&payload[8]);
    additional_count = read_be16(&payload[10]);
    total_records = (uint32_t)answer_count + (uint32_t)authority_count +
                    (uint32_t)additional_count;
    if ((flags & DNS_FLAG_RESPONSE) == 0u ||
        (flags & DNS_OPCODE_MASK) != 0u || question_count != 1u) {
        return malformed_transaction(transaction);
    }
    if (!decode_name(payload, payload_length, &offset, question_name) ||
        payload_length - offset < 4u) {
        return malformed_transaction(transaction);
    }
    question_type = read_be16(&payload[offset]);
    question_class = read_be16(&payload[offset + 2u]);
    offset += 4u;
    if (!strings_equal(question_name, transaction->name) ||
        question_type != DNS_TYPE_A || question_class != DNS_CLASS_IN) {
        return malformed_transaction(transaction);
    }
    /* A truncated UDP response is expected to omit some records even when the
     * header advertises them.  Once its matching question is authenticated,
     * report TC as a transport-level outcome instead of misclassifying the
     * absent tail as malformed. */
    if ((flags & DNS_FLAG_TRUNCATED) != 0u) {
        initialize_result(&result, NET_DNS_STATUS_TRUNCATED,
                          transaction->name);
        deliver_result(transaction, &result);
        return NET_DNS_RX_DELIVERED;
    }
    if (total_records > DNS_MAX_RR_COUNT) {
        return malformed_transaction(transaction);
    }

    bytes_zero(cnames, sizeof(cnames));
    bytes_zero(addresses, sizeof(addresses));
    for (section = 0u; section < 3u; ++section) {
        uint16_t records = section == 0u
                               ? answer_count
                               : (section == 1u ? authority_count
                                                : additional_count);
        uint16_t record_index;

        for (record_index = 0u; record_index < records; ++record_index) {
            char owner[NET_DNS_MAX_NAME_LENGTH + 1u];
            uint16_t type;
            uint16_t record_class;
            uint32_t ttl_seconds;
            uint16_t data_length;
            size_t data_offset;
            size_t data_end;

            if (!decode_name(payload, payload_length, &offset, owner) ||
                payload_length - offset < 10u) {
                return malformed_transaction(transaction);
            }
            type = read_be16(&payload[offset]);
            record_class = read_be16(&payload[offset + 2u]);
            ttl_seconds = read_be32(&payload[offset + 4u]);
            data_length = read_be16(&payload[offset + 8u]);
            offset += 10u;
            if ((size_t)data_length > payload_length - offset) {
                return malformed_transaction(transaction);
            }
            data_offset = offset;
            data_end = offset + data_length;

            if (section == 0u && type == DNS_TYPE_A &&
                record_class == DNS_CLASS_IN) {
                if (data_length != 4u ||
                    address_record_count >= DNS_MAX_A_RECORDS) {
                    return malformed_transaction(transaction);
                }
                string_copy(addresses[address_record_count].owner, owner,
                            sizeof(addresses[address_record_count].owner));
                addresses[address_record_count].address =
                    read_be32(&payload[data_offset]);
                addresses[address_record_count].ttl_seconds = ttl_seconds;
                ++address_record_count;
            } else if (section == 0u && type == DNS_TYPE_CNAME &&
                       record_class == DNS_CLASS_IN) {
                size_t name_offset = data_offset;

                if (cname_count >= DNS_MAX_CNAME_RECORDS ||
                    !decode_name(payload, payload_length, &name_offset,
                                 cnames[cname_count].target) ||
                    name_offset != data_end) {
                    return malformed_transaction(transaction);
                }
                string_copy(cnames[cname_count].owner, owner,
                            sizeof(cnames[cname_count].owner));
                cnames[cname_count].ttl_seconds = ttl_seconds;
                ++cname_count;
            }
            offset = data_end;
        }
    }
    if (offset != payload_length) {
        return malformed_transaction(transaction);
    }

    if ((flags & DNS_RCODE_MASK) == DNS_RCODE_NXDOMAIN) {
        initialize_result(&result, NET_DNS_STATUS_NXDOMAIN,
                          transaction->name);
        deliver_result(transaction, &result);
        return NET_DNS_RX_DELIVERED;
    }
    if ((flags & DNS_RCODE_MASK) != DNS_RCODE_NOERROR) {
        initialize_result(&result, NET_DNS_STATUS_SERVER_ERROR,
                          transaction->name);
        deliver_result(transaction, &result);
        return NET_DNS_RX_DELIVERED;
    }

    initialize_result(&result, NET_DNS_STATUS_OK, transaction->name);
    result.ttl_seconds = UINT32_MAX;
    for (;;) {
        size_t index;
        bool found_cname = false;
        size_t selected_cname = 0u;

        for (index = 0u; index < address_record_count; ++index) {
            if (strings_equal(addresses[index].owner,
                              result.canonical_name)) {
                size_t existing;
                bool duplicate = false;

                for (existing = 0u; existing < result.address_count;
                     ++existing) {
                    if (result.addresses[existing] == addresses[index].address) {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate && result.address_count < NET_DNS_MAX_ADDRESSES) {
                    result.addresses[result.address_count++] =
                        addresses[index].address;
                }
                if (addresses[index].ttl_seconds < result.ttl_seconds) {
                    result.ttl_seconds = addresses[index].ttl_seconds;
                }
            }
        }
        if (result.address_count != 0u) {
            break;
        }
        for (index = 0u; index < cname_count; ++index) {
            if (strings_equal(cnames[index].owner, result.canonical_name)) {
                if (found_cname &&
                    !strings_equal(cnames[selected_cname].target,
                                   cnames[index].target)) {
                    return malformed_transaction(transaction);
                }
                found_cname = true;
                selected_cname = index;
            }
        }
        if (!found_cname) {
            result.status = NET_DNS_STATUS_NODATA;
            result.ttl_seconds = 0u;
            break;
        }
        if (cnames[selected_cname].traversed) {
            return malformed_transaction(transaction);
        }
        cnames[selected_cname].traversed = true;
        if (cnames[selected_cname].ttl_seconds < result.ttl_seconds) {
            result.ttl_seconds = cnames[selected_cname].ttl_seconds;
        }
        string_copy(result.canonical_name, cnames[selected_cname].target,
                    sizeof(result.canonical_name));
    }
    if (result.address_count != 0u && result.ttl_seconds == UINT32_MAX) {
        result.ttl_seconds = 0u;
    }
    cache_result(client, &result, now_ms);
    deliver_result(transaction, &result);
    return NET_DNS_RX_DELIVERED;
}

void net_dns_poll(struct net_dns_client *client, uint64_t now_ms)
{
    size_t index;

    if (client == NULL) {
        return;
    }
    for (index = 0u; index < NET_DNS_MAX_CACHE_ENTRIES; ++index) {
        if (client->cache[index].in_use &&
            now_ms >= client->cache[index].expires_at_ms) {
            client->cache[index].in_use = false;
        }
    }
    for (index = 0u; index < NET_DNS_MAX_TRANSACTIONS; ++index) {
        struct net_dns_transaction *transaction = &client->transactions[index];

        if (!transaction->in_use) {
            continue;
        }
        if (now_ms >= transaction->deadline_at_ms) {
            struct net_dns_result result;

            initialize_result(&result, NET_DNS_STATUS_TIMEOUT,
                              transaction->name);
            deliver_result(transaction, &result);
            continue;
        }
        if (now_ms < transaction->next_retry_at_ms ||
            transaction->attempts >= NET_DNS_MAX_RETRIES) {
            continue;
        }
        (void)send_query(client, transaction);
        ++transaction->attempts;
        transaction->next_retry_at_ms = add_saturated(
            now_ms, DNS_RETRY_BASE_MS
                        << (uint8_t)(transaction->attempts - 1u));
    }
}

void net_dns_flush_cache(struct net_dns_client *client)
{
    if (client != NULL) {
        bytes_zero(client->cache, sizeof(client->cache));
    }
}
