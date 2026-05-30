#include <ruby/ruby.hpp>

#undef deprecated
#undef mkdir
#undef noinline
#undef usleep

using namespace nall;
using namespace ruby;

#include <ruby/video/video.cpp>
#include <ruby/audio/audio.cpp>
#include <ruby/input/input.cpp>
