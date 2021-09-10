#ifndef __GST_META_PES_HEADER_H__
#define __GST_META_PES_HEADER_H__

#include <gst/gst.h>
#include <gst/gstmeta.h>

G_BEGIN_DECLS

typedef struct _GstMetaPesHeader GstMetaPesHeader;
typedef struct _GstMetaPesHeaderInitParams GstMetaPesHeaderInitParams;

struct _GstMetaPesHeader {
    GstMeta meta;
    //TBD: GstClockTime timestamp;
    guint length;
    guchar* header;
};

struct _GstMetaPesHeaderInitParams {
    guint length;
    guchar* header;
};

GType gst_meta_pes_header_api_get_type (void);
const GstMetaInfo* gst_meta_pes_header_get_info (void);
#define GST_META_PES_HEADER_GET(buf) ((GstMetaPesHeader *)gst_buffer_get_meta(buf,gst_meta_pes_header_api_get_type()))
#define GST_META_PES_HEADER_ADD(buf,init_params) ((GstMetaPesHeader *)gst_buffer_add_meta(buf,gst_meta_pes_header_get_info(),(gpointer)&init_params))

G_END_DECLS

#endif //#ifndef __GST_META_PES_HEADER_H__
