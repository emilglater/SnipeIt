#include "wind.h"

/* Standard library includes */
#include <stddef.h>

/* User library includes */
#include "util/active_object/active_object.h"
#include "ddl/wind/wind_config.h"
#include "ddl/wind/wind_fsm.h"
#include "ddl/wind/wind_types.h"
#include "osal/osal.h"

static WindObject wind_aobj;

eStatus ddl_wind_init(DDLFrame* frame)
{
    if(frame == NULL)
    {
        return eSTATUS_NULL_PARAM;
    }

    wind_aobj.frame = &frame->wind_frame;
    wind_aobj.retry = 0;
    wind_aobj.current_target = eWIND_TARGET_SPEED;

    return util_active_object_init(
        &wind_aobj.aobj,
        eWIND_QUEUE_CAPACITY,
        wind_init_state
    );
}

eStatus ddl_wind_post(Event* event)
{
    return util_active_object_post(&wind_aobj.aobj, event);
}

eStatus ddl_wind_end(void)
{
    return util_active_object_end(&wind_aobj.aobj);
}

void ddl_wind_join(void)
{
    util_active_object_join(&wind_aobj.aobj);
}

void ddl_wind_delete(void)
{
    util_active_object_delete(&wind_aobj.aobj);
    osal_timer_destroy(wind_aobj.timer);
}