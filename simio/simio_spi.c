/* MSPDebug - debugging tool for MSP430 MCUs
 * Copyright (C) 2009, 2010 Daniel Beer
 *
 * This program is free software; you can redisgibute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is disgibuted in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <stdlib.h>
#include <string.h>
#include <czmq.h>

#include "simio_device.h"
#include "simio_spi.h"
#include "expr.h"
#include "output.h"

#define READ 0
#define WRITE 1


struct spi {
	struct simio_device	base;

  /* the number of the RXIFG bit in IFG2 */
  uint8_t interrupt_bit ;

	/* Rx register address */
	address_t	rx_addr;

  /* Rx data storage */
  uint8_t rx_reg ;

	/* Tx register address */
	address_t tx_addr;

  /* keep track of the last operation performed */
  int last_op ;

  /* socket for connecting to data provider */
  zsock_t* sock ;

};

static struct simio_device *spi_create(char **arg_text)
{
	struct spi *g;

	(void)arg_text;

	g = malloc(sizeof(*g));
	if (!g) {
		printc_err("spi: can't allocate memory");
		return NULL;
	}

	memset(g, 0, sizeof(*g));
	g->base.type = &simio_spi;
  // default config is for UCB0
  g->interrupt_bit = 2 ;  // 2nd bit is 0x04
	g->rx_addr = 0x006E ;
	g->tx_addr = 0x006F ;
  g->rx_reg = 0x00 ;
	g->sock = zsock_new_req("ipc:///tmp/simio_UCB0.sock");
  assert(g->sock);

	return (struct simio_device *)g;
}

static void spi_destroy(struct simio_device *dev)
{
	struct spi *g = (struct spi *)dev;
  zsock_destroy(&g->sock);
	free(g);
}

static void spi_reset(struct simio_device *dev)
{
	struct spi *g = (struct spi *)dev;
  simio_sfr_modify(SIMIO_IFG2, 1 << g->interrupt_bit, 0) ;
  g->rx_reg = 0x00 ;
  g->last_op = READ ;
}

static int config_addr(address_t *addr, char **arg_text)
{
	char *text = get_arg(arg_text);

	if (!text) {
		printc_err("spi: config: expected address\n");
		return -1;
	}

	if (expr_eval(text, addr) < 0) {
		printc_err("spi: can't parse address: %s\n", text);
		return -1;
	}

	return 0;
}

static int config_irq(uint8_t *irq, char **arg_text)
{
	char *text = get_arg(arg_text);
	address_t value;

	if (!text) {
		printc_err("spi: config: expected interrupt number\n");
		return -1;
	}

	if (expr_eval(text, &value) < 0) {
		printc_err("spi: can't parse interrupt number: %s\n", text);
		return -1;
	}

	*irq = value;
	return 0;
}

static int config_endpoint(struct spi *g, char **arg_text)
{
	char *endpoint = get_arg(arg_text);

	if (!endpoint) {
		printc_err("spi: config: expected endpoint\n");
		return -1;
	}

  zsock_t* tmp_ptr = zsock_new_req(endpoint) ;

  if(!tmp_ptr) {
    printc_err("spi: config: bad endpoint\n") ;
    return -1 ;
  }

  zsock_destroy(&g->sock) ;
  g->sock = tmp_ptr ;

	return 0;
}

static int spi_config(struct simio_device *dev,
			const char *param, char **arg_text)
{
	struct spi *g = (struct spi *)dev;

	if (!strcasecmp(param, "rx"))
		return config_addr(&g->rx_addr, arg_text);

	if (!strcasecmp(param, "tx"))
		return config_addr(&g->tx_addr, arg_text);

	if (!strcasecmp(param, "irq_bit"))
		return config_irq(&g->interrupt_bit, arg_text);

	if (!strcasecmp(param, "endpoint"))
		return config_endpoint(g, arg_text);

	printc_err("spi: config: unknown parameter: %s\n", param);
	return -1;
}


static int spi_info(struct simio_device *dev)
{
	struct spi *g = (struct spi *)dev;

	printc("Rx address:          0x%04x\n", g->rx_addr);
	printc("Tx address:          0x%04x\n", g->tx_addr);
	printc("RxIFG mask :         0x%02x\n", 1 << g->interrupt_bit);

	printc("0MQ data endpoint:   %s\n", zsock_last_endpoint(g->sock));
	printc("last op:             %c\n", g->last_op?'R':'W');
	printc("Rx value:            0x%02x\n", g->rx_reg);
	printc("\n");

	return 0;
}


static int spi_write_b(struct simio_device *dev,
			address_t addr, uint8_t data)
{
	struct spi *g = (struct spi *)dev;


  if(addr == g->tx_addr) {
    zsock_send(g->sock, "1", data) ;
    // even if we haven't actually received any
    // data on the socket yet we set the interrupt flag anyway
    uint8_t irq = 1 << g->interrupt_bit ;
    simio_sfr_modify(SIMIO_IFG2, irq, irq) ;
    g->last_op = WRITE ;
  }

	return 1;
}

static int spi_read_b(struct simio_device *dev,
		       address_t addr, uint8_t *data)
{
	struct spi *g = (struct spi *)dev;

  if(addr == g->rx_addr) {
    if(WRITE == g->last_op) {
      // should have data pending on the socket
      zsock_recv(g->sock, "1", &g->rx_reg) ;
    }
    *data = g->rx_reg ;
    g->last_op = READ ;
    return 0 ;
  }

	return 1;
}

const struct simio_class simio_spi = {
	.name = "spi",
	.help =
"This peripheral implements a digital IO port, with optional interrupt\n"
"functionality.\n"
"\n"
"Config arguments are:\n"
"    base <address>\n"
"        Set the peripheral base address.\n"
"    irq <interrupt>\n"
"        Set the interrupt vector for input pin state changes.\n"
"    noirq\n"
"        Disable interrupt functionality.\n"
"    verbose\n"
"        Print a message when output states change.\n"
"    quiet\n"
"        Don't print messages as output state changes.\n"
"    set <pin> <0|1>\n"
"        Set input pin state.\n",

	.create			= spi_create,
	.destroy		= spi_destroy,
	.reset			= spi_reset,
	.config			= spi_config,
	.info			= spi_info,
	.write_b		= spi_write_b,
	.read_b			= spi_read_b,
};
