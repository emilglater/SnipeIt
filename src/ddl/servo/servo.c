#include "servo.h"

/* Standard library includes */
#include <stddef.h>

/* User library includes */
#include "util/active_object/active_object.h"
#include "ddl/servo/servo_config.h"
#include "ddl/servo/servo_fsm.h"
#include "osal/osal.h"

static ServoObject servo_aobj;

eStatus ddl_servo_init(DDLFrame* frame)
{
    if(frame == NULL)
    {
        return eSTATUS_NULL_PARAM;
    }

    servo_aobj.frame = &frame->servo_frame;

    return util_active_object_init(
        &servo_aobj.aobj,
        eSERVO_QUEUE_CAPACITY,
        servo_init_state
    );
}

eStatus ddl_servo_post(Event* event)
{
    return util_active_object_post(&servo_aobj.aobj, event);
}

eStatus ddl_servo_end(void)
{
    return util_active_object_end(&servo_aobj.aobj);
}

void ddl_servo_join(void)
{
    util_active_object_join(&servo_aobj.aobj);
}

void ddl_servo_delete(void)
{
    util_active_object_delete(&servo_aobj.aobj);
    osal_timer_destroy(servo_aobj.timer);
    servo_fsm_destroy();
}

eStatus ddl_servo_set_target(float hor_angle, float ver_angle)
{
    return servo_fsm_set_target(hor_angle, ver_angle);
}

eStatus ddl_servo_get_pose(float* hor_angle, float* ver_angle)
{
    return servo_fsm_get_pose(hor_angle, ver_angle);
}