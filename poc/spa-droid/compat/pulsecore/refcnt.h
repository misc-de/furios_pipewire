#pragma once
#define PA_REFCNT_DECLARE   int _ref
#define PA_REFCNT_INIT(p)   ((p)->_ref = 1)
#define PA_REFCNT_INC(p)    ((p)->_ref++)
#define PA_REFCNT_DEC(p)    (--((p)->_ref))
#define PA_REFCNT_VALUE(p)  ((p)->_ref)
