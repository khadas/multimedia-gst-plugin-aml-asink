#############################################################
#
# gat-plugin-aml-asink
#
#############################################################
GST_PLUGIN_AML_ASINK_VERSION = 1.0
GST_PLUGIN_AML_ASINK_SITE = $(TOPDIR)/../multimedia/gst-plugin-aml-asink
GST_PLUGIN_AML_ASINK_SITE_METHOD = local

GST_PLUGIN_AML_ASINK_INSTALL_STAGING = YES
GST_PLUGIN_AML_ASINK_AUTORECONF = YES
GST_PLUGIN_AML_ASINK_DEPENDENCIES = gstreamer1 gst1-plugins-base host-pkgconf hal_audio_service speexdsp

$(eval $(autotools-package))

