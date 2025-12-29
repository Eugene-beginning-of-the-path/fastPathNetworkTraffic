// main.cpp
// Демонстрация “всего процесса”:
// 1) rte_eal_init() — DPDK подхватывает устройства (те, что заранее забинжены на vfio-pci/uio)
// 2) DPDK присваивает каждому доступному ethdev свой port_id (0..N-1)
// 3) Ты создаёшь mempool (rte_pktmbuf_pool_create) с n mbuf-ов
// 4) Ты настраиваешь порт на нужное число RX/TX очередей (rte_eth_dev_configure)
// 5) Ты “прикрепляешь” mempool к каждой RX-очереди (rte_eth_rx_queue_setup(..., mb_pool))
// 6) Запускаешь порт (rte_eth_dev_start) и начинаешь rte_eth_rx_burst()

extern "C" {
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
}

#include <cstdio>
#include <cstdlib>
#include <cstring>

static constexpr uint16_t NB_RXQ       = 2;        // сколько RX-очередей на порт
static constexpr uint16_t NB_TXQ       = 2;        // сколько TX-очередей на порт
static constexpr uint16_t RX_RING_SIZE = 1024;     // дескрипторов в RX ring на очередь
static constexpr uint16_t TX_RING_SIZE = 1024;     // дескрипторов в TX ring на очередь
static constexpr uint16_t BURST_SIZE   = 32;

static constexpr uint32_t MBUF_CACHE_SIZE = 256;

// Сколько mbuf-ов в пуле (n). Минимум нужно >= sum(RX_RING_SIZE по всем rx-очередям всех портов)
// плюс запас на "in-flight" пакеты в обработке.
static uint32_t calc_num_mbufs(uint16_t nb_ports) {
    const uint32_t rx_need = nb_ports * NB_RXQ * RX_RING_SIZE;
    const uint32_t tx_need = nb_ports * NB_TXQ * TX_RING_SIZE; // не строго обязательно, но полезный запас
    const uint32_t slack   = 8192; // общий запас на обработку/очереди/пики
    return rx_need + tx_need + slack;
}

static void port_init(uint16_t port_id, rte_mempool* mbuf_pool) {
    rte_eth_conf port_conf{};
    port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_RSS; // можно убрать, если RSS не нужен

    // 1) Конфигурируем порт: сколько RX/TX очередей
    int rc = rte_eth_dev_configure(port_id, NB_RXQ, NB_TXQ, &port_conf);
    if (rc < 0) {
        std::fprintf(stderr, "rte_eth_dev_configure(port=%u) failed: %s\n",
                     port_id, rte_strerror(-rc));
        std::exit(1);
    }

    const int socket_id = rte_eth_dev_socket_id(port_id);

    // 2) Setup RX очередей: вот здесь ты "крепишь mempool к RX-очереди"
    for (uint16_t q = 0; q < NB_RXQ; q++) {
        rc = rte_eth_rx_queue_setup(
            port_id,
            q,
            RX_RING_SIZE,
            socket_id,
            /*rx_conf*/ nullptr,
            /*mb_pool*/ mbuf_pool
        );
        if (rc < 0) {
            std::fprintf(stderr, "rte_eth_rx_queue_setup(port=%u, q=%u) failed: %s\n",
                         port_id, q, rte_strerror(-rc));
            std::exit(1);
        }
    }

    // 3) Setup TX очередей (mempool сюда не передаётся)
    for (uint16_t q = 0; q < NB_TXQ; q++) {
        rc = rte_eth_tx_queue_setup(
            port_id,
            q,
            TX_RING_SIZE,
            socket_id,
            /*tx_conf*/ nullptr
        );
        if (rc < 0) {
            std::fprintf(stderr, "rte_eth_tx_queue_setup(port=%u, q=%u) failed: %s\n",
                         port_id, q, rte_strerror(-rc));
            std::exit(1);
        }
    }

    // 4) Стартуем порт
    rc = rte_eth_dev_start(port_id);
    if (rc < 0) {
        std::fprintf(stderr, "rte_eth_dev_start(port=%u) failed: %s\n",
                     port_id, rte_strerror(-rc));
        std::exit(1);
    }

    // (необязательно) включить promiscuous
    rte_eth_promiscuous_enable(port_id);
}

int main(int argc, char** argv) {
    // ---------- ШАГ 1: EAL init ----------
    // Здесь DPDK поднимает окружение, hugepages, IOVA режим, драйверы,
    // и формирует список доступных ethdev (те, что забинжены на DPDK-драйвер).
    int rc = rte_eal_init(argc, argv);
    if (rc < 0) {
        std::fprintf(stderr, "rte_eal_init failed\n");
        return 1;
    }

    // Сдвигаем argc/argv, если ты дальше парсишь свои аргументы
    argc -= rc;
    argv += rc;

    // ---------- ШАГ 2: DPDK выдаёт port_id ----------
    const uint16_t nb_ports = rte_eth_dev_count_avail();
    if (nb_ports == 0) {
        std::fprintf(stderr, "No DPDK eth ports available (is NIC bound to vfio-pci/uio?)\n");
        return 1;
    }

    std::printf("DPDK sees %u eth ports:\n", nb_ports);
    for (uint16_t port_id = 0; port_id < nb_ports; port_id++) {
        char name[RTE_ETH_NAME_MAX_LEN]{};
        if (rte_eth_dev_get_name_by_port(port_id, name) == 0) {
            std::printf("  port_id=%u name=%s\n", port_id, name);
        } else {
            std::printf("  port_id=%u (name unavailable)\n", port_id);
        }
    }

    // ---------- ШАГ 3: Создаём mempool с n mbuf-ов ----------
    // n = число элементов в пуле, т.е. "сколько mbuf-ов туда поместится" = n.
    const uint32_t NUM_MBUFS = calc_num_mbufs(nb_ports);

    // Типичный data_room_size: RTE_MBUF_DEFAULT_BUF_SIZE (включает headroom).
    // Можно поставить своё (например 2048 + headroom), если знаешь что делаешь.
    const uint16_t DATA_ROOM_SIZE = RTE_MBUF_DEFAULT_BUF_SIZE;

    rte_mempool* mbuf_pool = rte_pktmbuf_pool_create(
        "MBUF_POOL",
        NUM_MBUFS,
        MBUF_CACHE_SIZE,
        /*priv_size*/ 0,
        DATA_ROOM_SIZE,
        rte_socket_id()
    );
    if (!mbuf_pool) {
        std::fprintf(stderr, "rte_pktmbuf_pool_create failed: %s\n", rte_strerror(rte_errno));
        return 1;
    }

    std::printf("Created mempool: n=%u mbufs, data_room=%u bytes\n", NUM_MBUFS, DATA_ROOM_SIZE);

    // ---------- ШАГ 4–6: Конфигурируем порт и крепим mempool к RX-очередям ----------
    for (uint16_t port_id = 0; port_id < nb_ports; port_id++) {
        port_init(port_id, mbuf_pool);
        std::printf("Port %u started with %u RX queues, %u TX queues\n", port_id, NB_RXQ, NB_TXQ);
    }

    // ---------- Пример RX цикла ----------
    // Берём пакеты из rx-ring (rx-queue 0), получаем rte_mbuf* и читаем "сырые байты" из data buffer.
    // (Здесь просто освобождаем.)
    std::printf("Entering RX loop on lcore %u...\n", rte_lcore_id());

    rte_mbuf* bufs[BURST_SIZE];

    while (true) {
        for (uint16_t port_id = 0; port_id < nb_ports; port_id++) {
            const uint16_t nb_rx = rte_eth_rx_burst(port_id, /*queue_id*/ 0, bufs, BURST_SIZE);
            if (nb_rx == 0) continue;

            for (uint16_t i = 0; i < nb_rx; i++) {
                rte_mbuf* m = bufs[i];

                // "сырые байты" (начало L2 обычно здесь):
                // uint8_t* pkt = rte_pktmbuf_mtod(m, uint8_t*);
                // дальше парсишь ether/ip/udp/tcp и т.д.

                rte_pktmbuf_free(m);
            }
        }
    }

    // сюда обычно не доходим
    return 0;
}
