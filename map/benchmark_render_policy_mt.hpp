#pragma once

#include "render_policy_mt.hpp"

class BenchmarkRenderPolicyMT : public RenderPolicyMT
{
public:
  BenchmarkRenderPolicyMT(shared_ptr<WindowHandle> const & wh,
                          RenderPolicy::TRenderFn const & renderFn);

  void Initialize(shared_ptr<yg::gl::RenderContext> const & rc,
                  shared_ptr<yg::ResourceManager> const & rm);

  void DrawFrame(shared_ptr<PaintEvent> const & e, ScreenBase const & s);
};
