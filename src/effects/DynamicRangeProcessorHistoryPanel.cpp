/*  SPDX-License-Identifier: GPL-2.0-or-later */
/*!********************************************************************

  Audacity: A Digital Audio Editor

  DynamicRangeProcessorHistoryPanel.cpp

  Matthieu Hodgkinson

**********************************************************************/
#include "DynamicRangeProcessorHistoryPanel.h"
#include "AColor.h"
#include "AllThemeResources.h"
#include "CompressorInstance.h"
#include "DynamicRangeProcessorHistory.h"
#include "DynamicRangeProcessorPanelCommon.h"
#include "Theme.h"
#include "widgets/LinearDBFormat.h"
#include "widgets/LinearUpdater.h"
#include "widgets/Ruler.h"
#include <cassert>
#include <wx/dcclient.h>
#include <wx/graphics.h>

namespace
{
constexpr auto timerId = 7000;
// Of course we aren't really targetting 200fps, but when specifying 50fps, we
// rather get 30fps, with outliers at 20. Measurements (Windows) showed that,
// when specifying 200, we get around 60fps on average, with outlier around 40.
constexpr auto timerPeriodMs = 1000 / 200;

static const wxColor inputColor { 142, 217, 115, 144 };
static const wxColor outputColor { 103, 124, 228 };

float GetDbRange(int height)
{
   const auto factor = std::max(
      1.f, 1.f * height / DynamicRangeProcessorHistoryPanel::minHeight);
   return factor * DynamicRangeProcessorHistoryPanel::minRangeDb;
}
} // namespace

BEGIN_EVENT_TABLE(DynamicRangeProcessorHistoryPanel, wxPanelWrapper)
EVT_PAINT(DynamicRangeProcessorHistoryPanel::OnPaint)
EVT_SIZE(DynamicRangeProcessorHistoryPanel::OnSize)
EVT_TIMER(timerId, DynamicRangeProcessorHistoryPanel::OnTimer)
END_EVENT_TABLE()

DynamicRangeProcessorHistoryPanel::DynamicRangeProcessorHistoryPanel(
   wxWindow* parent, wxWindowID winid, CompressorInstance& instance,
   std::function<void(float)> onDbRangeChanged)
    : wxPanelWrapper { parent, winid }
    , mCompressorInstance { instance }
    , mOnDbRangeChanged { std::move(onDbRangeChanged) }
    , mInitializeProcessingSettingsSubscription { static_cast<
                                                     InitializeProcessingSettingsPublisher&>(
                                                     instance)
                                                     .Subscribe(
                                                        [&](const std::optional<
                                                            InitializeProcessingSettings>&
                                                               evt) {
                                                           if (evt)
                                                              InitializeForPlayback(
                                                                 instance,
                                                                 evt->sampleRate);
                                                           else
                                                              // Stop the
                                                              // timer-based
                                                              // update but keep
                                                              // the history
                                                              // until playback
                                                              // is resumed.
                                                              mTimer.Stop();
                                                        }) }
    , mRealtimeResumeSubscription {
       static_cast<RealtimeResumePublisher&>(instance).Subscribe([this](auto) {
          if (mHistory)
             mHistory->BeginNewSegment();
       })
    }
{
   if (const auto& sampleRate = instance.GetSampleRate();
       sampleRate.has_value())
      // Playback is ongoing, and so the `InitializeProcessingSettings` event
      // was already fired.
      InitializeForPlayback(instance, *sampleRate);

   SetDoubleBuffered(true);
   mTimer.SetOwner(this, timerId);
   SetSize({ minWidth, minHeight });
}

namespace
{
double GetDisplayPixel(float elapsedSincePacket, int panelWidth)
{
   const auto secondsPerPixel =
      1. * DynamicRangeProcessorHistory::maxTimeSeconds / panelWidth;
   // A display delay to avoid the display to tremble near time zero because the
   // data hasn't arrived yet.
   // This is a trade-off between visual comfort and timely update. It was set
   // empirically, but with a relatively large audio playback delay. Maybe it
   // will be found to lag on lower-latency playbacks. Best would probably be to
   // make it playback-delay dependent.
   constexpr auto displayDelay = 0.2f;
   return panelWidth - 1 -
          (elapsedSincePacket - displayDelay) / secondsPerPixel;
}

/*!
 * Wherever `A` and `B` cross, evaluates the exact x and y crossing position and
 * adds a point to `A` and `B`.
 * @pre `A.size() == B.size()`
 * @post `A.size() == B.size()`
 */
void InsertCrossings(
   std::vector<wxPoint2DDouble>& A, std::vector<wxPoint2DDouble>& B)
{
   assert(A.size() == B.size());
   if (A.size() != B.size())
      return;
   std::optional<bool> aWasBelow;
   auto x0 = 0.;
   auto y0_a = 0.;
   auto y0_b = 0.;
   auto it = A.begin();
   auto jt = B.begin();
   while (it != A.end())
   {
      const auto x2 = it->m_x;
      const auto y2_a = it->m_y;
      const auto y2_b = jt->m_y;
      const auto aIsBelow = y2_a < y2_b;
      if (aWasBelow.has_value() && *aWasBelow != aIsBelow)
      {
         // clang-format off
         // We have a crossing of y2_a and y2_b between x0 and x2.
         //    y_a(x) = y0_a + (x - x0) / (x2 - x0) * (y2_a - y0_a)
         // and likewise for y_b.
         // Let y_a(x1) = y_b(x1) and solve for x1:
         // x1 = x0 + (x2 - x0) * (y0_b - y0_a) / ((a_n - y0_a) - (b_n - y0_b))
         // clang-format on
         const auto x1 =
            x0 + (x2 - x0) * (y0_a - y0_b) / (y2_b - y0_b + y0_a - y2_a);
         const auto y = y0_a + (x1 - x0) / (x2 - x0) * (y2_a - y0_a);
         it = A.emplace(it, x1, y)++;
         jt = B.emplace(jt, x1, y)++;
      }
      x0 = x2;
      y0_a = y2_a;
      y0_b = y2_b;
      aWasBelow = aIsBelow;
      ++it;
      ++jt;
   }
}

/*!
 * Fills the area between the lines and the bottom of the panel with the given
 * color.
 */
template <typename Brush>
void FillUpTo(
   std::vector<wxPoint2DDouble> lines, const Brush& brush,
   wxGraphicsContext& gc, const wxSize& size)
{
   const auto height = size.GetHeight();
   const auto left = std::max<double>(0., lines.front().m_x);
   const auto right = std::min<double>(size.GetWidth(), lines.back().m_x);
   auto area = gc.CreatePath();
   area.MoveToPoint(right, height);
   area.AddLineToPoint(left, height);
   std::for_each(lines.begin(), lines.end(), [&area](const auto& p) {
      area.AddLineToPoint(p);
   });
   area.CloseSubpath();
   gc.SetBrush(brush);
   gc.FillPath(area);
}

/*!
 * Fills the above `base` and below `line` with the given color.
 * @pre `base.size() == line.size()`
 */
void FillExcess(
   const std::vector<wxPoint2DDouble>& line, std::vector<wxPoint2DDouble> base,
   const wxColor& color, wxPaintDC& dc)
{
   const auto gc = DynamicRangeProcessorPanel::MakeGraphicsContext(dc);
   // transform `base` in-place to the lower of the two lines.
   auto& lower = base;
   std::transform(
      line.begin(), line.end(), base.begin(), lower.begin(),
      [](const auto& f, const auto& t) {
         return wxPoint2DDouble { f.m_x, std::max(f.m_y, t.m_y) };
      });
   wxGraphicsPath area = gc->CreatePath();
   area.MoveToPoint(lower.front());
   std::for_each(lower.begin(), lower.end(), [&area](const auto& p) {
      area.AddLineToPoint(p);
   });
   std::for_each(line.rbegin(), line.rend(), [&area](const auto& p) {
      area.AddLineToPoint(p);
   });
   area.CloseSubpath();

   gc->SetBrush(wxBrush { color });
   gc->FillPath(area);
}

void DrawLegend(size_t height, wxPaintDC& dc, wxGraphicsContext& gc)
{
   using namespace DynamicRangeProcessorPanel;

   constexpr auto legendWidth = 10;
   constexpr auto legendHeight = 10;
   constexpr auto legendSpacing = 5;
   constexpr auto legendX = 5;
   const auto legendY = height - 5 - legendHeight;
   const auto legendTextX = legendX + legendWidth + legendSpacing;
   const auto legendTextHeight = dc.GetTextExtent("X").GetHeight();
   const auto legendTextYOffset = (legendHeight - legendTextHeight) / 2;
   const auto legendTextY = legendY + legendTextYOffset;

   struct LegendInfo
   {
      const wxColor color;
      const TranslatableString text;
   };

   std::vector<LegendInfo> legends = {
      { inputColor, XO("Input") },
      { outputColor, XO("Output") },
      /* i18n-hint: when smoothing leads the output level to be momentarily
       * over the target */
      { attackColor, XO("Overshoot") },
      /* i18n-hint: when smoothing leads the output level to be momentarily
       * under the target */
      { releaseColor, XO("Undershoot") }
   };

   int legendTextXOffset = 0;
   gc.SetPen(lineColor);
   dc.SetTextForeground(*wxBLACK);
   dc.SetFont(
      { 8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL });
   for (const auto& legend : legends)
   {
      // First fill with background color so that transparent foreground colors
      // yield the same result as on the graph.
      gc.SetBrush(backgroundColor);
      gc.DrawRectangle(
         legendX + legendTextXOffset, legendY, legendWidth, legendHeight);
      gc.SetBrush(legend.color);
      gc.DrawRectangle(
         legendX + legendTextXOffset, legendY, legendWidth, legendHeight);

      dc.DrawText(
         legend.text.Translation(), legendTextX + legendTextXOffset,
         legendTextY);
      const auto legendTextWidth =
         dc.GetTextExtent(legend.text.Translation()).GetWidth();
      legendTextXOffset +=
         legendWidth + legendSpacing + legendTextWidth + legendSpacing;
   }

   // Add a legend entry for the compression line:
   gc.SetPen(lineColor);
   const auto compressionLineX = legendX + legendTextXOffset + legendSpacing;
   const auto compressionLineY = legendY + legendHeight / 2;
   gc.StrokeLine(
      compressionLineX, compressionLineY, compressionLineX + legendWidth,
      compressionLineY);
   const auto compressionText = XO("Compression");
   const auto compressionTextWidth =
      dc.GetTextExtent(compressionText.Translation()).GetWidth();
   dc.DrawText(
      compressionText.Translation(), compressionLineX + legendWidth + 5,
      legendTextY);
}
} // namespace

void DynamicRangeProcessorHistoryPanel::ShowInput(bool show)
{
   mShowInput = show;
   Refresh(false);
}

void DynamicRangeProcessorHistoryPanel::ShowOutput(bool show)
{
   mShowOutput = show;
   Refresh(false);
}

void DynamicRangeProcessorHistoryPanel::ShowOvershoot(bool show)
{
   mShowOvershoot = show;
   Refresh(false);
}

void DynamicRangeProcessorHistoryPanel::ShowUndershoot(bool show)
{
   mShowUndershoot = show;
   Refresh(false);
}

void DynamicRangeProcessorHistoryPanel::OnPaint(wxPaintEvent& evt)
{
   wxPaintDC dc(this);

   using namespace DynamicRangeProcessorPanel;

   const auto gc = MakeGraphicsContext(dc);
   const auto width = GetSize().GetWidth();
   const auto height = GetSize().GetHeight();

   gc->SetBrush(gc->CreateLinearGradientBrush(
      0, 0, 0, height, backgroundColor, *wxWHITE));
   gc->SetPen(wxTransparentColor);
   gc->DrawRectangle(0, 0, width - 1, height - 1);

   Finally Do { [&] {
      DrawLegend(height, dc, *gc);
      gc->SetBrush(*wxTRANSPARENT_BRUSH);
      gc->SetPen(lineColor);
      gc->DrawRectangle(0, 0, width - 1, height - 1);
   } };

   if (!mHistory || !mSync)
   {
      if (!mPlaybackAboutToStart)
      {
         const auto text = XO("awaiting playback");
         const wxDCFontChanger changer { dc,
                                         { 16, wxFONTFAMILY_DEFAULT,
                                           wxFONTSTYLE_NORMAL,
                                           wxFONTWEIGHT_NORMAL } };
         const auto textWidth = dc.GetTextExtent(text.Translation()).GetWidth();
         const auto textHeight =
            dc.GetTextExtent(text.Translation()).GetHeight();
         dc.SetTextForeground(wxColor { 128, 128, 128 });
         dc.DrawText(
            text.Translation(), (width - textWidth) / 2,
            (height - textHeight) / 2);
      }
      return;
   }

   const auto& segments = mHistory->GetSegments();
   const auto elapsedTimeSinceFirstPacket =
      std::chrono::duration<float>(mSync->now - mSync->start).count();
   const auto rangeDb = GetDbRange(height);
   const auto dbPerPixel = rangeDb / height;

   for (const auto& segment : segments)
   {
      mX.clear();
      mTarget.clear();
      mActual.clear();
      mInput.clear();
      mOutput.clear();

      mX.resize(segment.size());
      auto lastInvisibleLeft = 0;
      auto firstInvisibleRight = 0;
      std::transform(
         segment.begin(), segment.end(), mX.begin(), [&](const auto& packet) {
            const auto x = GetDisplayPixel(
               elapsedTimeSinceFirstPacket -
                  (packet.time - mSync->firstPacketTime),
               width);
            if (x < 0)
               ++lastInvisibleLeft;
            if (x < width)
               ++firstInvisibleRight;
            return x;
         });
      lastInvisibleLeft = std::max<int>(--lastInvisibleLeft, 0);
      firstInvisibleRight = std::min<int>(++firstInvisibleRight, mX.size());

      mX.erase(mX.begin() + firstInvisibleRight, mX.end());
      mX.erase(mX.begin(), mX.begin() + lastInvisibleLeft);

      if (mX.size() < 2)
         continue;

      auto segmentIndex = lastInvisibleLeft;
      mTarget.reserve(mX.size());
      mActual.reserve(mX.size());
      mInput.reserve(mX.size());
      mOutput.reserve(mX.size());
      std::for_each(mX.begin(), mX.end(), [&](auto x) {
         const auto& packet = segment[segmentIndex++];
         const auto elapsedSincePacket = elapsedTimeSinceFirstPacket -
                                         (packet.time - mSync->firstPacketTime);
         mTarget.emplace_back(x, -packet.target / dbPerPixel);
         mActual.emplace_back(x, -packet.follower / dbPerPixel);
         mInput.emplace_back(x, -packet.input / dbPerPixel);
         mOutput.emplace_back(x, -packet.output / dbPerPixel);
      });

      if (mShowOutput)
      {
         // Paint output first with opaque color.
         constexpr auto w = .4;
         // Circle color.
         const wxColor cCol = GetColorMix(backgroundColor, outputColor, w);
         const auto xo = width * 0.9; // "o" for "origin"
         const auto yo = height * 0.1;
         const auto xf = width * 0.5; // "f" for "focus"
         const auto yf = height * 0.2;
         const auto radius = width;
         const auto brush = gc->CreateRadialGradientBrush(
            xo, yo, xf, yf, radius, outputColor, cCol);
         FillUpTo(mOutput, brush, *gc, GetSize());
      }

      if (mShowInput)
         FillUpTo(mInput, inputColor, *gc, GetSize());

      if (mShowOvershoot || mShowUndershoot)
      {
         // We have to paint the difference between these two lines, and in
         // different colors depending on which is on top. To fill the correct
         // polygons, we have to add points where the lines intersect.
         InsertCrossings(mActual, mTarget);
         if (mShowOvershoot)
            FillExcess(mActual, mTarget, attackColor, dc);
         if (mShowUndershoot)
            FillExcess(mTarget, mActual, releaseColor, dc);
      }

      // Actual compression line
      const auto gc2 = MakeGraphicsContext(dc);
      gc2->SetPen(lineColor);
      gc2->DrawLines(mActual.size(), mActual.data());
   }
}

void DynamicRangeProcessorHistoryPanel::OnSize(wxSizeEvent& evt)
{
   Refresh(false);
   mOnDbRangeChanged(GetDbRange(GetSize().GetHeight()));
}

void DynamicRangeProcessorHistoryPanel::OnTimer(wxTimerEvent& evt)
{
   mPacketBuffer.clear();
   DynamicRangeProcessorOutputPacket packet;
   while (mOutputQueue->Get(packet))
      mPacketBuffer.push_back(packet);
   mHistory->Push(mPacketBuffer);

   if (mHistory->IsEmpty())
      return;

   // Do now get `std::chrono::steady_clock::now()` in the `OnPaint` event,
   // because this can be triggered even when playback is paused.
   const auto now = std::chrono::steady_clock::now();
   if (!mSync)
      // At the time of writing, the realtime playback doesn't account for
      // varying latencies. When it does, the synchronization will have to be
      // updated on latency change. See
      // https://github.com/audacity/audacity/issues/3223#issuecomment-2137025150.
      mSync.emplace(
         ClockSynchronization { mHistory->GetSegments().front().front().time +
                                   mCompressorInstance.GetLatencyMs() / 1000,
                                now });
   mPlaybackAboutToStart = false;

   mSync->now = now;

   Refresh(false);
   wxPanelWrapper::Update();
}

void DynamicRangeProcessorHistoryPanel::InitializeForPlayback(
   CompressorInstance& instance, double sampleRate)
{
   mSync.reset();
   mHistory.emplace(sampleRate);
   // We don't know for sure the least packet size (which is variable). 100
   // samples per packet at a rate of 8kHz is 12.5ms, which is quite low
   // latency. For higher sample rates that will be less.
   constexpr auto leastPacketSize = 100;
   const size_t maxQueueSize = DynamicRangeProcessorHistory::maxTimeSeconds *
                               sampleRate / leastPacketSize;
   mPacketBuffer.reserve(maxQueueSize);
   // Although `mOutputQueue` is a shared_ptr, we construct a unique_ptr and
   // invoke the shared_ptr ctor overload that takes a unique_ptr.
   // This way, we avoid the `error: aligned deallocation function of type 'void
   // (void *, std::align_val_t) noexcept' is only available on macOS 10.13 or
   // newer` compilation error.
   mOutputQueue =
      std::make_unique<DynamicRangeProcessorOutputPacketQueue>(maxQueueSize);
   instance.SetOutputQueue(mOutputQueue);
   mTimer.Start(timerPeriodMs);
   mPlaybackAboutToStart = true;
   Refresh(false);
   wxPanelWrapper::Update();
}

bool DynamicRangeProcessorHistoryPanel::AcceptsFocus() const
{
   return false;
}

bool DynamicRangeProcessorHistoryPanel::AcceptsFocusFromKeyboard() const
{
   return false;
}
