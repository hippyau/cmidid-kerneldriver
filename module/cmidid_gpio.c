#define DEBUG
#include <linux/gpio.h>
#include <linux/slab.h>
#include <linux/stat.h>
#include <linux/interrupt.h>
#include <linux/moduleparam.h>

#include "cmidid_main.h"
#include "cmidid_gpio.h"
#include "cmidid_midi.h"

/*
 * GPIOs requested via kernel parameter.
 * This is a simple list of gpio ids with an corresponding pitch.
 */
static int gpio_mapping[MAX_REQUEST];
static int gpio_mapping_size;
module_param_array(gpio_mapping, int, &gpio_mapping_size, 0);
MODULE_PARM_DESC(gpio_mapping,
		 "Mapping of GPIOs to Keys. Format: gpio1, gpio2, note, ...");

#define START_BUTTON 0
#define END_BUTTON 1

typedef enum {
	KEY_INACTIVE,
	KEY_TOUCHED,
	KEY_PRESSED
} KEY_STATE;

struct key {
	KEY_STATE state;
	struct gpio gpios[2];	/* GPIO port for first trigger. */
	unsigned int irqs[2];
	s64 hit_time[2];
	int note;
	int last_velocity;
};

struct cmidid_gpio_state {
	struct key *keys;
	int num_keys;
	bool button_active_high[2];
};

int get_key_from_irq(int irq, struct key **k_ret, unsigned char *button_ret);

struct cmidid_gpio_state state;

static void handle_button_event(int irq, bool active)
{
	struct key *k;
	unsigned char button;

	if (get_key_from_irq(irq, &k, &button) < 0) {
		dbg("irq not assigned to key\n");
		return;
	}

	switch (k->state) {
	case KEY_INACTIVE:
		if ((button = START_BUTTON) && active) {
			//start counting
			k->state = KEY_TOUCHED;
		} else if ((button = START_BUTTON) && !active) {
			//noteoff
		}
		break;
	case KEY_TOUCHED:
		if ((button = START_BUTTON) && !active) {
			//noteoff
			k->state = KEY_INACTIVE;
		} else if ((button = END_BUTTON) && active) {
			//calculate velocity
			//noteon
			k->state = KEY_PRESSED;
		}
		break;
	case KEY_PRESSED:
		if ((button = START_BUTTON) && !active) {
			//noteoff
			k->state = KEY_INACTIVE;
		} else if ((button = END_BUTTON) && active) {
			//repeat noteon
		}
		break;
	default:
		//noteoff
		k->state = KEY_INACTIVE;
	}
}

int get_key_from_irq(int irq, struct key **k_ret, unsigned char *button_ret)
{
	struct key *k;

	for (k = state.keys; k < state.keys + state.num_keys; k++) {
		if (k->irqs[START_BUTTON] == irq) {
			*k_ret = k;
			*button_ret = START_BUTTON;
			return 0;
		}

		if (k->irqs[END_BUTTON] == irq) {
			*k_ret = k;
			*button_ret = END_BUTTON;
			return 0;
		}
	}

	return -1;
}

/*
 * The interrupt handler for every GPIO interrupt.
 */
static irqreturn_t irq_handler(int irq, void *dev_id)
{
	//struct key *k;
	info("Interrupt handler called %d: %p.\n", irq, dev_id);
	/*k = get_key_from_gpio(irq);

	   if (k != NULL) {
	   send_note(k->note, 100);
	   } */

	return IRQ_HANDLED;
}

static bool is_valid(int gpio)
{
	int i;
	for (i = 0; i < state.num_keys; i++) {
		if (gpio == state.keys[i].gpios[START_BUTTON].gpio) {
			dbg("gpio: %d is invalid. It was already used...\n",
			    gpio);
			return false;
		}
		if (gpio == state.keys[i].gpios[END_BUTTON].gpio) {
			dbg("gpio: %d is invalid. It was already used...\n",
			    gpio);
			return false;
		}
	}

	return gpio_is_valid(gpio);
}

int gpio_init(void)
{
	int err;
	int i;
	struct key *k;
	unsigned int irq;

	dbg("GPIO component initializing...\n");

	/* Drop if the array length is invalid. */
	if (gpio_mapping_size <= 0) {
		err("No GPIO_Mapping specified\n");
		return -EINVAL;
	}

	/* Drop if array length is not a multiple of three. */
	if (gpio_mapping_size % 3 != 0) {
		err("Invalid GPIO-Mapping. Argument number not a multiple of 3. Format: gpio1, gpio2, key, ...\n");
		return -EINVAL;
	}

	/* Allocate one button struct for every pair of gpios with pitch. */
	state.num_keys = gpio_mapping_size / 3;
	state.keys = kzalloc(state.num_keys * sizeof(struct key), GFP_KERNEL);

	if (state.keys == NULL) {
		err("Failed to allocate memory\n");
		return -ENOMEM;
	}

	for (i = 0; i < state.num_keys; i++) {
		k = state.keys + i;

		k->state = KEY_INACTIVE;

		if (!is_valid(gpio_mapping[3 * i + START_BUTTON])) {
			err("Invalid gpio: %d\n",
			    gpio_mapping[3 * i + START_BUTTON]);
			err = -EINVAL;
			goto free_buttons;
		}
		k->gpios[START_BUTTON].gpio =
		    gpio_mapping[3 * i + START_BUTTON];
		k->gpios[START_BUTTON].flags = GPIOF_IN;
		k->gpios[START_BUTTON].label = "NO_LABEL";

		if (!is_valid(gpio_mapping[3 * i + END_BUTTON])) {
			err("Invalid gpio: %d\n",
			    gpio_mapping[3 * i + END_BUTTON]);
			err = -EINVAL;
			goto free_buttons;
		}
		k->gpios[END_BUTTON].gpio = gpio_mapping[3 * i + END_BUTTON];
		k->gpios[END_BUTTON].flags = GPIOF_IN;
		k->gpios[END_BUTTON].label = "NO_LABEL";

		k->note = gpio_mapping[3 * i + 2];

		dbg("Setting key: gpio_start = %d, gpio_end = %d, note = %d\n",
		    k->gpios[START_BUTTON].gpio, k->gpios[END_BUTTON].gpio,
		    k->note);

		if ((err = gpio_request_array(k->gpios, 2)) < 0) {
			err("Could not request gpio %d or %d.",
			    k->gpios[START_BUTTON].gpio,
			    k->gpios[END_BUTTON].gpio);
			goto free_buttons;
		}

		irq = gpio_to_irq(k->gpios[START_BUTTON].gpio);
		if (irq < 0) {
			err("Could not request irq for gpio %d.\n",
			    k->gpios[START_BUTTON].gpio);
			err = irq;
			goto free_buttons;
		}
		k->irqs[START_BUTTON] = irq;

		irq = gpio_to_irq(k->gpios[END_BUTTON].gpio);
		if (irq < 0) {
			err("Could not request irq for gpio %d.\n",
			    k->gpios[END_BUTTON].gpio);
			err = irq;
			goto free_buttons;
		}
		k->irqs[END_BUTTON] = irq;

		if ((err =
		     request_irq(k->irqs[START_BUTTON], irq_handler,
				 IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
				 "irq_start", NULL)) < 0) {
			err("Could not request irq for key.\n");
			err = -EINVAL;
			goto free_buttons;
		}

		if ((err =
		     request_irq(k->irqs[END_BUTTON], irq_handler,
				 IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
				 "irq_start", NULL)) < 0) {
			err("Could not request irq for key.\n");
			err = -EINVAL;
			goto free_buttons;
		}
	}

	return 0;

 free_buttons:

	for (--i; i > 0; --i) {
		gpio_free_array(state.keys[i].gpios, 2);
	}

	kfree(state.keys);

	return err;
}

void gpio_exit(void)
{
	int i;
	dbg("GPIO component exiting...\n");

	for (i = 0; i < state.num_keys; i++) {
		free_irq(state.keys[i].irqs[START_BUTTON], NULL);
		free_irq(state.keys[i].irqs[END_BUTTON], NULL);
		gpio_free_array(state.keys[i].gpios, 2);
	}

	kfree(state.keys);
}
