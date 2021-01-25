/**
 * @file  s1.c
 * @brief S1 Module Core Functions
 *        
 *        Various functions to setup and configure the
 *        S1 Module. To access these functions, use the
 *        s1.h header file.
 * 
 * @attention (c) 2021 Silicon Witchery 
 *            (info@siliconwitchery.com)
 *
 *        Licensed under a Creative Commons Attribution 
 *        4.0 International License. This code is provided
 *        as-is and no warranty is given.
*/

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <math.h>


#include "nrf_gpio.h"
#include "nrfx_saadc.h"
#include "nrfx_spim.h"
#include "nrfx_twim.h"
#include "nrf52811.h"
#include "s1.h"

/**
 * @brief Pinout definition for the nRF52811 chip
 *        on the S1 Module.
 * 
 *        This isn't the pinout of the module itself,
 *        but rather the internal connections between
 *        the nRF, PMIC, flash IC and FPGA.
 */
#define ADC1_PIN            NRF_SAADC_INPUT_AIN2
#define ADC2_PIN            NRF_SAADC_INPUT_AIN3
#define PMIC_AMUX_PIN       NRF_SAADC_INPUT_AIN1

#define SPI_SI_PIN          NRF_GPIO_PIN_MAP(0, 8)
#define SPI_SO_PIN          NRF_GPIO_PIN_MAP(0, 11)
#define SPI_CS_PIN          NRF_GPIO_PIN_MAP(0, 12)
#define SPI_CLK_PIN         NRF_GPIO_PIN_MAP(0, 15)
#define FPGA_RESET_PIN      NRF_GPIO_PIN_MAP(0, 20)
#define FPGA_DONE_PIN       NRF_GPIO_PIN_MAP(0, 16)

#define PMIC_SDA_PIN        NRF_GPIO_PIN_MAP(0, 14)
#define PMIC_SCL_PIN        NRF_GPIO_PIN_MAP(0, 17)
#define PMIC_I2C_ADDRESS    0x48


// Instances for I2C and SPI
static const nrfx_spim_t spi = NRFX_SPIM_INSTANCE(0);
static const nrfx_twim_t i2c = NRFX_TWIM_INSTANCE(0);

static uint8_t pmic_read_reg(uint8_t reg)
{
    uint8_t rx_buffer;
    nrfx_twim_xfer_desc_t i2c_xfer = NRFX_TWIM_XFER_DESC_TXRX(PMIC_I2C_ADDRESS, &reg, 1, &rx_buffer, 1);
    APP_ERROR_CHECK(nrfx_twim_xfer(&i2c, &i2c_xfer, 0));
    return rx_buffer;
}

static void pmic_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t buffer[2];
    nrfx_twim_xfer_desc_t i2c_xfer = NRFX_TWIM_XFER_DESC_TX(0x48, buffer, 2);
    buffer[0] = reg;
    buffer[1] = value;
    APP_ERROR_CHECK(nrfx_twim_xfer(&i2c, &i2c_xfer, 0));
}

static void flash_tx_rx(uint8_t * tx_buffer, size_t tx_len,
                        uint8_t * rx_buffer, size_t rx_len)
{
    // SPI hardware configuration
    nrfx_spim_config_t spi_config = NRFX_SPIM_DEFAULT_CONFIG;
    spi_config.mosi_pin = SPI_SO_PIN;
    spi_config.miso_pin = SPI_SI_PIN;
    spi_config.sck_pin = SPI_CLK_PIN;
    spi_config.ss_pin = SPI_CS_PIN;

    // Initialise the SPI if it was not already
    nrfx_spim_init(&spi, &spi_config, NULL, NULL);

    nrfx_spim_xfer_desc_t spi_xfer = NRFX_SPIM_XFER_TRX(tx_buffer, tx_len, 
                                                        rx_buffer, rx_len);

    APP_ERROR_CHECK(nrfx_spim_xfer(&spi, &spi_xfer, 0));
}

s1_error_t s1_pmic_set_vaux(float voltage)
{
    // Check if voltage is a valid range
    if (voltage < 0.8 || voltage > 5.5)
    {
        return S1_INVALID_SETTING;
    }

    // If voltage > than 3.46, then LDO0 must not
    // be in LSW mode.
    if (voltage > 3.46 && (pmic_read_reg(0x39) & 0x08))
    {
        return S1_INVALID_SETTING;
    }

    // If 0V, we shutdown the SSB2
    if (voltage == 0.0)
    {
        pmic_write_reg(0x2E, 0x0C);
        return S1_SUCCESS;
    }

    // Set LDO target voltage
    uint8_t voltage_setting = (voltage - 0.8) / 0.05;
    pmic_write_reg(0x2D, voltage_setting);

    // Enable SSB2
    // - Buck Boost mode
    // - Discharge resistor enable
    // - 1A limit
    pmic_write_reg(0x2E, 0x0E);

    return S1_SUCCESS;
}

s1_error_t s1_pmic_set_vio(float voltage)
{
    // Check if voltage is a valid range
    // 3.46V limit is to protect FPGA
    if (voltage < 0.8 || voltage > 3.46)
    {
        return S1_INVALID_SETTING;
    }

    // If 0V, we shutdown the LDO
    if (voltage == 0.0)
    {
        pmic_write_reg(0x39, 0x0C);
        return S1_SUCCESS;
    }

    // Set LDO target voltage
    uint8_t voltage_setting = (voltage - 0.8) / 0.025;
    pmic_write_reg(0x38, voltage_setting);

    // Enable LDO0
    // - LDO regulator mode
    // - Discharge resistor active
    // - Enable
    pmic_write_reg(0x39, 0x0E);

    return S1_SUCCESS;
}

void s1_pimc_fpga_vcore(bool enable)
{
    // Ensure SSB1 is 1.2V
    pmic_write_reg(0x2B, 0x08);

    // If enable
    if (enable)
    {
        // Enable SSB1
        // - 0.333A limit
        // - Buck mode
        pmic_write_reg(0x2C, 0x7E);
        return;
    }

    // Disable LDO0 (Vio). Required to avoid
    // IO voltages damaging the FPGA core.
    pmic_write_reg(0x39, 0x0C);

    // Disable SSB1 (Vfpga)
    pmic_write_reg(0x2C, 0x7C);
}

s1_error_t s1_flash_wakeup(void)
{
    // Wake up the flash
    uint8_t wake_seq[4] = {0xAB, 0, 0, 0};
    flash_tx_rx((uint8_t*)&wake_seq, 4, NULL, 0);
    NRFX_DELAY_US(3); // tRES1 required to come out of sleep

    // Reset sequence has to happen as two transfers
    uint8_t reset_seq[3] = {0x66, 0x99};
    flash_tx_rx((uint8_t*)&reset_seq, 1, NULL, 0);
    flash_tx_rx((uint8_t*)&reset_seq+1, 1, NULL, 0);
    NRFX_DELAY_US(30); // tRST to fully reset

    // Check if the capacity ID corresponds to 32M
    uint8_t cap_id_reg[1] = {0x9F};
    uint8_t cap_id_res[4] = {0};
    flash_tx_rx((uint8_t*)&cap_id_reg, 1, (uint8_t*)&cap_id_res, 4);

    LOG("Flash capacity = 0x%x", cap_id_res[3]); //should be 0x16
    if (cap_id_res[3] != 0x16)
    {
        return S1_FLASH_ERROR;
    }

    return S1_SUCCESS;
}

void s1_flash_erase_all(void)
{
    // Issue erase sequence
    uint8_t erase_seq[2] = {0x06, 0x60};
    flash_tx_rx((uint8_t*)&erase_seq, 1, NULL, 0);
    flash_tx_rx((uint8_t*)&erase_seq+1, 1, NULL, 0);
}

bool s1_flash_is_busy(void)
{
    // Read status register
    uint8_t status_reg[1] = {0x05};
    uint8_t status_res[2] = {0};
    flash_tx_rx((uint8_t*)&status_reg, 1, (uint8_t*)&status_res, 2);

    // LSB of 0x05 register should clear once done
    LOG("Status: 0x%x", status_res[1]);

    if (!(status_res[1] & 0x01))
    {
        LOG("Erase Done!");
        return false;
    }

    return true;
}

void s1_fpga_hold_reset(void)
{
    nrf_gpio_pin_clear(FPGA_RESET_PIN);
}

s1_error_t s1_init(void)
{
    // FPGA control pins configuration
    // - reset pin as output (low signal holds FPGA in reset)
    // - done pin as input (goes high when FPGA is configured)
    nrf_gpio_cfg_output(FPGA_RESET_PIN);
    nrf_gpio_cfg_input(FPGA_DONE_PIN, NRF_GPIO_PIN_PULLUP);

    // I2C hardware configuration
    nrfx_twim_config_t pmic_twi_config = NRFX_TWIM_DEFAULT_CONFIG;
    pmic_twi_config.scl = PMIC_SCL_PIN;
    pmic_twi_config.sda = PMIC_SDA_PIN;
    APP_ERROR_CHECK(nrfx_twim_init(&i2c, &pmic_twi_config, NULL, NULL));
    nrfx_twim_enable(&i2c);

    // Check PMIC Chip ID
    if (pmic_read_reg(0x14) != 0x7A)
    {
        return S1_PMIC_ERROR;
    }

    // TODO setup analog pins here?

    return S1_SUCCESS;
}