#pragma once

#include <cassert>
#include <cstdint>
#include <vector>

constexpr uint8_t HSPI = 1;
constexpr uint8_t MSBFIRST = 1 , SPI_MODE0 = 0;
extern std::vector<uint8_t> host_rx , host_tx;
extern size_t host_rx_at;

struct SPISettings
{
    uint32_t frequency;
    uint8_t order , mode;

    SPISettings(uint32_t hz , uint8_t bit_order , uint8_t spi_mode) : frequency(hz) , order(bit_order) , mode(spi_mode)
    {
    }
};

class SPIClass
{
public:
    uint8_t bus;
    bool started = false;
    bool transaction = false;

    explicit SPIClass(uint8_t value) : bus(value)
    {
    }

    void begin(signed sck , signed miso , signed mosi , signed cs)
    {
        assert(bus == HSPI && sck == 18 && miso == 15 && mosi == 14 && cs == 21);
        started = true;
    }

    void end()
    {
        started = false;
    }

    void beginTransaction(SPISettings settings)
    {
        assert(started && !transaction);
        assert(settings.frequency == 100000 && settings.order == MSBFIRST && settings.mode == SPI_MODE0);
        transaction = true;
    }

    uint8_t transfer(uint8_t data)
    {
        assert(transaction && host_rx_at < host_rx.size());
        host_tx.push_back(data);
        return host_rx[host_rx_at ++];
    }

    void endTransaction()
    {
        assert(transaction);
        transaction = false;
    }
};
