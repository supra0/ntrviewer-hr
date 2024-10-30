#include "ntr_common.h"
#include "const.h"

#define NTR_IP_NAME_LEN_MAX (32)
#define NTR_IP_OCTET_SIZE (4)

#ifdef _WIN32
#include <winsock2.h>
#endif

int sock_startup(void)
{
#ifdef _WIN32
    WSADATA wsa_data;
    return WSAStartup(MAKEWORD(2, 2), &wsa_data);
#else
    return 0;
#endif
}

int sock_cleanup(void)
{
#ifdef _WIN32
    return WSACleanup();
#else
    return 0;
#endif
}

struct ntr_rp_config_t ntr_rp_config;
void ntr_config_set_default(void) {
    ntr_rp_config = (struct ntr_rp_config_t){
        .top_screen_priority = 1,
        .screen_priority_factor = 2,
        .jpeg_quality = 75,
        .bandwidth_limit = 16,
        .kcp_mode = 1,
    };
}

char **ntr_auto_ip_list;
uint8_t **ntr_auto_ip_octet_list;
int ntr_auto_ip_count;

static void ntr_free_auto_ip_list(void) {
    if(ntr_auto_ip_count) {
        for (int i = 0; i < ntr_auto_ip_count; ++i) {
            free(ntr_auto_ip_list[i]);
            free(ntr_auto_ip_octet_list[i]);
        }
        free(ntr_auto_ip_list);
        free(ntr_auto_ip_octet_list);
        ntr_auto_ip_list = 0;
        ntr_auto_ip_octet_list = 0;
        ntr_auto_ip_count = 0;
    }
}

static int ntr_alloc_auto_ip_list(int count) {
    if (count) {
        ntr_auto_ip_list = malloc(sizeof(*ntr_auto_ip_list) * count);
        if (!ntr_auto_ip_list) {
            err_log("malloc ntr_auto_ip_list failed\n");
            goto fail;
        }
        memset(ntr_auto_ip_list, 0, sizeof(*ntr_auto_ip_list) * count);

        ntr_auto_ip_octet_list = malloc(sizeof(*ntr_auto_ip_octet_list) * count);
        if (!ntr_auto_ip_octet_list) {
            err_log("malloc ntr_auto_ip_list failed\n");
            goto fail;
        }
        memset(ntr_auto_ip_octet_list, 0, sizeof(*ntr_auto_ip_octet_list) * count);

        for (int i = 0; i < count; ++i) {
            ntr_auto_ip_list[i] = malloc(NTR_IP_NAME_LEN_MAX);
            if (!ntr_auto_ip_list[i]) {
                err_log("malloc ntr_auto_ip_list[i] failed\n");
                goto fail;
            }

            ntr_auto_ip_octet_list[i] = malloc(NTR_IP_OCTET_SIZE);
            if (!ntr_auto_ip_octet_list[i]) {
                err_log("malloc ntr_auto_ip_octet_list[i] failed\n");
                goto fail;
            }
        }
        ntr_auto_ip_count = count;
    }
    return 0;

fail:
    if (ntr_auto_ip_list) {
        for (int i = 0; i < count; ++i) {
            if (ntr_auto_ip_list[i]) {
                free(ntr_auto_ip_list[i]);
            }
        }

        free(ntr_auto_ip_list);
        ntr_auto_ip_list = 0;
    }

    if (ntr_auto_ip_octet_list) {
        for (int i = 0; i < count; ++i) {
            if (ntr_auto_ip_octet_list[i]) {
                free(ntr_auto_ip_octet_list[i]);
            }
        }

        free(ntr_auto_ip_octet_list);
        ntr_auto_ip_octet_list = 0;
    }

    return -1;
}

int ntr_selected_ip;
int ntr_selected_adapter;

char **ntr_adaptor_list;
uint8_t **ntr_adaptor_octet_list;
int ntr_adaptor_count;

// taken from Boop's source code https://github.com/miltoncandelero/Boop
static uint8_t const known_mac_list[][3] = {
    { 0x00, 0x09, 0xBF }, { 0x00, 0x16, 0x56 }, { 0x00, 0x17, 0xAB }, { 0x00, 0x19, 0x1D }, { 0x00, 0x19, 0xFD },
    { 0x00, 0x1A, 0xE9 }, { 0x00, 0x1B, 0x7A }, { 0x00, 0x1B, 0xEA }, { 0x00, 0x1C, 0xBE }, { 0x00, 0x1D, 0xBC },
    { 0x00, 0x1E, 0x35 }, { 0x00, 0x1E, 0xA9 }, { 0x00, 0x1F, 0x32 }, { 0x00, 0x1F, 0xC5 }, { 0x00, 0x21, 0x47 },
    { 0x00, 0x21, 0xBD }, { 0x00, 0x22, 0x4C }, { 0x00, 0x22, 0xAA }, { 0x00, 0x22, 0xD7 }, { 0x00, 0x23, 0x31 },
    { 0x00, 0x23, 0xCC }, { 0x00, 0x24, 0x1E }, { 0x00, 0x24, 0x44 }, { 0x00, 0x24, 0xF3 }, { 0x00, 0x25, 0xA0 },
    { 0x00, 0x26, 0x59 }, { 0x00, 0x27, 0x09 }, { 0x04, 0x03, 0xD6 }, { 0x18, 0x2A, 0x7B }, { 0x2C, 0x10, 0xC1 },
    { 0x34, 0xAF, 0x2C }, { 0x40, 0xD2, 0x8A }, { 0x40, 0xF4, 0x07 }, { 0x58, 0x2F, 0x40 }, { 0x58, 0xBD, 0xA3 },
    { 0x5C, 0x52, 0x1E }, { 0x60, 0x6B, 0xFF }, { 0x64, 0xB5, 0xC6 }, { 0x78, 0xA2, 0xA0 }, { 0x7C, 0xBB, 0x8A },
    { 0x8C, 0x56, 0xC5 }, { 0x8C, 0xCD, 0xE8 }, { 0x98, 0xB6, 0xE9 }, { 0x9C, 0xE6, 0x35 }, { 0xA4, 0x38, 0xCC },
    { 0xA4, 0x5C, 0x27 }, { 0xA4, 0xC0, 0xE1 }, { 0xB8, 0x78, 0x26 }, { 0xB8, 0x8A, 0xEC }, { 0xB8, 0xAE, 0x6E },
    { 0xCC, 0x9E, 0x00 }, { 0xCC, 0xFB, 0x65 }, { 0xD8, 0x6B, 0xF7 }, { 0xDC, 0x68, 0xEB }, { 0xE0, 0x0C, 0x7F },
    { 0xE0, 0xE7, 0x51 }, { 0xE8, 0x4E, 0xCE }, { 0xEC, 0xC4, 0x0D }, { 0xE8, 0x4E, 0xCE }
};

static void ntr_free_adaptor_list(void) {
    if(ntr_adaptor_count) {
        for (int i = 0; i < ntr_adaptor_count; ++i) {
            free(ntr_adaptor_list[i]);
            free(ntr_adaptor_octet_list[i]);
        }
        free(ntr_adaptor_list);
        free(ntr_adaptor_octet_list);
        ntr_adaptor_list = 0;
        ntr_adaptor_octet_list = 0;
        ntr_adaptor_count = 0;
    }
}

static int ntr_alloc_adaptor_list(int count) {
    if (count) {
        ntr_adaptor_list = malloc(sizeof(*ntr_adaptor_list) * count);
        if (!ntr_adaptor_list) {
            err_log("malloc ntr_adaptor_list failed\n");
            goto fail;
        }
        memset(ntr_adaptor_list, 0, sizeof(*ntr_adaptor_list) * count);

        ntr_adaptor_octet_list = malloc(sizeof(*ntr_adaptor_octet_list) * count);
        if (!ntr_adaptor_octet_list) {
            err_log("malloc ntr_adaptor_octet_list failed\n");
            goto fail;
        }
        memset(ntr_adaptor_octet_list, 0, sizeof(*ntr_adaptor_octet_list) * count);

        for (int i = 0; i < count; ++i) {
            ntr_adaptor_list[i] = malloc(NTR_IP_NAME_LEN_MAX);
            if (!ntr_adaptor_list[i]) {
                err_log("malloc ntr_adaptor_list[i] failed\n");
                goto fail;
            }

            ntr_adaptor_octet_list[i] = malloc(NTR_IP_OCTET_SIZE);
            if (!ntr_adaptor_octet_list[i]) {
                err_log("malloc ntr_adaptor_octet_list[i] failed\n");
                goto fail;
            }
        }
        ntr_adaptor_count = count;
    }
    return 0;

fail:
    if (ntr_adaptor_list) {
        for (int i = 0; i < count; ++i) {
            if (ntr_adaptor_list[i]) {
                free(ntr_adaptor_list[i]);
            }
        }

        free(ntr_adaptor_list);
        ntr_adaptor_list = 0;
    }

    if (ntr_adaptor_octet_list) {
        for (int i = 0; i < count; ++i) {
            if (ntr_adaptor_octet_list[i]) {
                free(ntr_adaptor_octet_list[i]);
            }
        }

        free(ntr_adaptor_octet_list);
        ntr_adaptor_octet_list = 0;
    }

    return -1;
}

atomic_uint_fast8_t ntr_ip_octet[NTR_IP_OCTET_SIZE];

void ntr_try_auto_select_adaptor(void) {
    ntr_selected_adapter = 0;
    uint32_t count = 0;
    for (int i = NTR_ADAPTOR_PRE_COUNT; i < ntr_adaptor_count - NTR_ADAPTOR_POST_COUNT; ++i) {
        uint32_t bits = __builtin_bswap32(*(uint32_t *)ntr_ip_octet & *(uint32_t *)ntr_adaptor_octet_list[i]);
        if (/*(int)bits < 0 && */bits > count) {
            count = bits;
            ntr_selected_adapter = i;
        }
    }
}

#ifdef _WIN32
#include <iphlpapi.h>

static PMIB_IPNETTABLE ip_net_buf = 0;
static ULONG ip_net_buf_size = 0;

static void get_ip_map_mac(void) {
    if (ip_net_buf) {
        free(ip_net_buf);
        ip_net_buf = 0;
        ip_net_buf_size = 0;
    }

    ip_net_buf_size = 0;
    if (GetIpNetTable(NULL, &ip_net_buf_size, TRUE) == ERROR_INSUFFICIENT_BUFFER) {
        ip_net_buf = malloc(ip_net_buf_size);
        if (!ip_net_buf) {
            ip_net_buf_size = 0;
            err_log("malloc ip_net_buf failed\n");
            return;
        }
        ULONG ret = GetIpNetTable(ip_net_buf, &ip_net_buf_size, TRUE);
        if (ret == NO_ERROR) {
            return;
        } else {
            err_log("GetIpNetTable failed: %d\n", (int)ret);
            free(ip_net_buf);
            ip_net_buf = 0;
            ip_net_buf_size = 0;
        }
    } else {
        ip_net_buf_size = 0;
    }
}

static int match_mac(UCHAR *mac) {
    // err_log("%02x-%02x-%02x\n", (int)mac[0], (int)mac[1], (int)mac[2]);
    for (unsigned i = 0; i < sizeof(known_mac_list) / sizeof(*known_mac_list); ++i) {
        if (memcmp(mac, known_mac_list[i], 3) == 0)
            return 1;
    }
    return 0;
}

#define NTR_AUTO_IP_PRE_COUNT (1)

void ntr_detect_3ds_ip(void) {
    get_ip_map_mac();

    int detected_ip_count = 0;
    int *map_index = 0;
    if (ip_net_buf_size) {
        map_index = malloc(ip_net_buf->dwNumEntries * sizeof(*map_index));
        if (!map_index) {
            err_log("malloc map_index failed\n");
            goto fail;
        }
        for (unsigned i = 0; i < ip_net_buf->dwNumEntries; ++i) {
            PMIB_IPNETROW entry = &ip_net_buf->table[i];
            if (entry->dwType != MIB_IPNET_TYPE_INVALID) {
                if (entry->dwPhysAddrLen == 6) {
                    if (match_mac(entry->bPhysAddr)) {
                        map_index[detected_ip_count] = i;
                        ++detected_ip_count;
                    }
                }
            }
        }
    }

    ntr_free_auto_ip_list();
    if (ntr_alloc_auto_ip_list(detected_ip_count + NTR_AUTO_IP_PRE_COUNT)) {
        goto fail;
    }

    if (detected_ip_count) {
        strcpy(ntr_auto_ip_list[0], "");
    } else {
        strcpy(ntr_auto_ip_list[0], "None Detected");
    }
    memset(ntr_auto_ip_octet_list[0], 0, NTR_IP_OCTET_SIZE);

    for (int i = 0; i < detected_ip_count; ++i) {
        PMIB_IPNETROW entry = &ip_net_buf->table[map_index[i]];
        uint8_t *octets = (uint8_t *)&entry->dwAddr;
        sprintf(ntr_auto_ip_list[i + NTR_AUTO_IP_PRE_COUNT], "%d.%d.%d.%d", (int)octets[0], (int)octets[1], (int)octets[2], (int)octets[3]);
        memcpy(ntr_auto_ip_octet_list[i + NTR_AUTO_IP_PRE_COUNT], &entry->dwAddr, NTR_IP_OCTET_SIZE);
    }
    free(map_index);

    ntr_selected_ip = detected_ip_count ? NTR_AUTO_IP_PRE_COUNT : 0;
    memcpy(ntr_ip_octet, ntr_auto_ip_octet_list[ntr_selected_ip], NTR_IP_OCTET_SIZE);

    return;

fail:
    return;
}

static PIP_ADAPTER_INFO adapter_info_list;
static ULONG adapter_info_list_size;

static uint32_t parse_ip_address(const char *ip) {
    return inet_addr(ip);
}

static int get_adaptor_count(void) {
    int count = 0;
    if (adapter_info_list && adapter_info_list_size) {
        PIP_ADAPTER_INFO next = adapter_info_list;
        while (next) {
            PIP_ADDR_STRING ip = &next->IpAddressList;
            while (ip) {
                if (parse_ip_address(ip->IpAddress.String) != 0)
                  ++count;
                ip = ip->Next;
            }
            next = next->Next;
        }
    }
    return count;
}

static void update_adapter_list(void) {
    ntr_free_adaptor_list();

    int count = get_adaptor_count();

    if (ntr_alloc_adaptor_list(count + NTR_ADAPTOR_EXTRA_COUNT)) {
        goto fail;
    }

    strcpy(ntr_adaptor_list[0], "0.0.0.0 (Any)");
    memset(ntr_adaptor_octet_list[0], 0, NTR_IP_OCTET_SIZE);

    if (adapter_info_list && adapter_info_list_size) {
        PIP_ADAPTER_INFO next = adapter_info_list;
        for (int i = 0; i < count;) {
            PIP_ADDR_STRING ip = &next->IpAddressList;
            while (ip) {
                int addr;
                if ((addr = parse_ip_address(ip->IpAddress.String)) != 0) {
                    sprintf(ntr_adaptor_list[i + NTR_ADAPTOR_PRE_COUNT], "%s", ip->IpAddress.String);
                    memcpy(ntr_adaptor_octet_list[i + NTR_ADAPTOR_PRE_COUNT], &addr, NTR_IP_OCTET_SIZE);
                    ++i;
                }
                ip = ip->Next;
            }
            next = next->Next;
        }
    }

    strcpy(ntr_adaptor_list[NTR_ADAPTOR_PRE_COUNT + count + NTR_ADAPTOR_POST_AUTO], "Auto-Select");
    memset(ntr_adaptor_octet_list[1 + count], 0, NTR_IP_OCTET_SIZE);

    strcpy(ntr_adaptor_list[NTR_ADAPTOR_PRE_COUNT + count + NTR_ADAPTOR_POST_REFRESH], "Refresh List");
    memset(ntr_adaptor_octet_list[1 + count + 1], 0, NTR_IP_OCTET_SIZE);

    ntr_try_auto_select_adaptor();

    return;

fail:
    return;
}

void ntr_get_adapter_list(void) {
    if (adapter_info_list) {
        free(adapter_info_list);
        adapter_info_list = 0;
        adapter_info_list_size = 0;
    }

    ULONG ret = GetAdaptersInfo(adapter_info_list, &adapter_info_list_size);
    if (ret == ERROR_BUFFER_OVERFLOW) {
        adapter_info_list = malloc(adapter_info_list_size);
        if (!adapter_info_list) {
            err_log("malloc adapter_info_list failed\n");
            goto fail;
        }
        ret = GetAdaptersInfo(adapter_info_list, &adapter_info_list_size);
        if (ret == ERROR_SUCCESS) {
        } else {
            err_log("GetAdaptersInfo failed: %d\n", (int)ret);
            free(adapter_info_list);
            adapter_info_list = 0;
            adapter_info_list_size = 0;
        }
    } else if (ret != ERROR_SUCCESS) {
        err_log("GetAdaptersInfo failed: %d\n", (int)ret);
        adapter_info_list_size = 0;
    }

    update_adapter_list();
    return;

fail:
    return;
}
#else
// TODO
#endif
