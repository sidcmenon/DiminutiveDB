#include "khabibdb.h"

const char *khb_strerror(khb_status s)
{
    switch (s) {
    case KHB_OK:           return "KHB_OK";
    case KHB_ERR_IO:       return "KHB_ERR_IO";
    case KHB_ERR_CORRUPT:  return "KHB_ERR_CORRUPT";
    case KHB_ERR_LOCKED:   return "KHB_ERR_LOCKED";
    case KHB_ERR_NOTFOUND: return "KHB_ERR_NOTFOUND";
    case KHB_ERR_EXISTS:   return "KHB_ERR_EXISTS";
    case KHB_ERR_NOMEM:    return "KHB_ERR_NOMEM";
    case KHB_ERR_INVALID:  return "KHB_ERR_INVALID";
    case KHB_ERR_FULL:     return "KHB_ERR_FULL";
    case KHB_ERR_STATE:    return "KHB_ERR_STATE";
    }
    return "KHB_ERR_UNKNOWN";
}