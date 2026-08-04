#include "active_object.h"

/* Standard libraries */
#include <stddef.h>

/* The entry function with which we start the Active Object thread*/
static void* active_entry(void* arg)
{
    ActiveObject* active_object = (ActiveObject*)arg;
    void* event;

    /* Initialize the FSM */
    (void)util_fsm_init(&active_object->active_fsm, active_object->init_state, arg);

    /* FSM event loop */
    for(;;)
    {
        (void)util_queue_pop(&active_object->event_queue, &event);
        if(((Event*)event)->type == eFSM_EVENT_END)
        {
            break;
        }

        (void)util_fsm_send_event(&active_object->active_fsm, event);
    }

    /* ---- NOT REACHABLE ---- */
    return NULL;
}

eStatus util_active_object_init(ActiveObject* active_object, uint32_t capacity, StateFP init_state)
{
    if(active_object == NULL || init_state == NULL)
    {
        return eSTATUS_NULL_PARAM;
    }

    active_object->init_state = init_state;

    if(util_queue_init(&active_object->event_queue, capacity))
    {
        return eSTATUS_SYSTEM_ERROR;
    }

    if(osal_thread_create(&active_object->thread, active_entry, active_object))
    {
        util_queue_delete(&active_object->event_queue);
        return eSTATUS_SYSTEM_ERROR;
    }

    return eSTATUS_SUCCESSFUL;
}

eStatus util_active_object_post(ActiveObject* active_object, Event* event)
{
    if(active_object == NULL || event == NULL)
    {
        return eSTATUS_NULL_PARAM;
    }

    eStatus status = util_queue_push(&active_object->event_queue, event);
    if(status == eSTATUS_ACTION_FAILED || status != eSTATUS_SUCCESSFUL)
    {
        return eSTATUS_ACTION_FAILED;
    }

    return eSTATUS_SUCCESSFUL;
}

eStatus util_active_object_end(ActiveObject* active_object)
{
    static Event end_event = { .type = eFSM_EVENT_END };
    return util_active_object_post(active_object, &end_event);
}

void util_active_object_join(ActiveObject* active_object)
{
    osal_thread_join(active_object->thread);
}

void util_active_object_delete(ActiveObject* active_object)
{
    util_queue_delete(&active_object->event_queue);
}