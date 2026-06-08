#include "compass.h"

/* User library includes */
#include "util/active_object/active_object.h"
#include "ddl/compass/compass_config.h"
#include "ddl/compass/compass_fsm.h"
#include "osal/osal.h"

static CompassObject compass_aobj;
 
eStatus ddl_compass_init(DDLFrame* frame)
{
    if(frame == NULL)
    {
        return eSTATUS_NULL_PARAM;
    }
 
    compass_aobj.frame = &frame->mag_frame;
    compass_aobj.retry = 0;
 
    return util_active_object_init(
        &compass_aobj.aobj,
        eCOMPASS_QUEUE_CAPACITY,
        compass_init_state
    );
}
 
eStatus ddl_compass_post(Event* event)
{
    return util_active_object_post(&compass_aobj.aobj, event);
}
 
eStatus ddl_compass_end(void)
{
    return util_active_object_end(&compass_aobj.aobj);
}
 
void ddl_compass_join(void)
{
    util_active_object_join(&compass_aobj.aobj);
}
 
void ddl_compass_delete(void)
{
    util_active_object_delete(&compass_aobj.aobj);
}
