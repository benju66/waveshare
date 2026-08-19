#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Owns the answer to "who is drawing on the panel right now".
//
// Exactly one renderer owns the panel at a time: the fluid renderer on
// PAGE_FLUID, LVGL on every other page. Transitions run on the page manager's
// own task, because parking the outgoing renderer and draining its last DMA
// transfer can take tens of milliseconds and must not stall touch handling.

typedef enum {
    PAGE_FLUID = 0,
    PAGE_TIMER,
    PAGE_NOW,
    PAGE_SETTINGS,
    PAGE_COUNT,
} page_id_t;

typedef enum {
    PAGE_EVT_SWIPE_LEFT,   // finger travelled left: advance to the next page
    PAGE_EVT_SWIPE_RIGHT,  // finger travelled right: back to the previous page
    PAGE_EVT_WAKE,         // touch or PWR press while dimmed/off: bring it back
    PAGE_EVT_SCREEN_OFF,   // PWR short press: panel off, everything parked
    PAGE_EVT_ALERT_TIMER,  // a pomodoro phase ended: wake and show the timer
} page_evt_t;

// Sets up state and starts the page manager task. Boot page is PAGE_FLUID.
// Call before the render/sim/LVGL tasks exist, since their gates read this
// module's state.
esp_err_t page_manager_init(void);

// Hands over the task handles the manager notifies to unpark. Call once the
// tasks have been created; transitions are impossible before the first touch
// event, which is long after.
void page_manager_register_tasks(TaskHandle_t render_task, TaskHandle_t sim_task,
                                 TaskHandle_t lvgl_task);

// From the touch task. Never blocks; drops the event if the queue is full.
void page_manager_post_event(page_evt_t evt);

// From the touch task, on every sample with a finger down: feeds the idle
// timer. Call wake_touch on the press's first sample; it returns true when
// that press was consumed to wake an idle-dimmed screen, in which case the
// touch task should not let the rest of the contact reach LVGL.
void page_manager_note_activity(void);
bool page_manager_wake_touch(void);

bool page_manager_screen_is_off(void);

bool page_manager_fluid_active(void);
bool page_manager_lvgl_active(void);
page_id_t page_manager_current(void);

// Park gates, called at the top of each renderer's loop. While the page the
// renderer draws is inactive the gate acks that the loop body is not running
// (so no snapshot mutex held, no band buffer acquired) and sleeps until the
// manager notifies. Costs one atomic read on the hot path.
void page_manager_render_gate(void);
void page_manager_sim_gate(void);
void page_manager_lvgl_gate(void);
