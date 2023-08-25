#include <stdio.h>
#include <gst/gst.h>
#include <gstparam_time_pair.h>

int main(int argc, char **argv)
{
  gint64 pts = 0;
  guint64 mono = 0;
  GValue test = {0,};

  gst_init (&argc, &argv);

  gst_param_spec_time_pair_get_type ();
  g_value_init (&test, GST_TYPE_TIME_PAIR);

  gst_value_set_time_pair (&test, 1000, 1000);

  pts = gst_value_get_time_pair_pos (&test);
  mono = gst_value_get_time_pair_mono (&test);

  printf("pts %lld mono %lld\n", pts, mono);
  return 0;
}
