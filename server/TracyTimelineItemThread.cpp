#include <algorithm>
#include <limits>

#include "TracyImGui.hpp"
#include "TracyMouse.hpp"
#include "TracyPrint.hpp"
#include "TracyTimelineContext.hpp"
#include "TracyTimelineItemThread.hpp"
#include "TracyView.hpp"
#include "TracyWorker.hpp"

namespace tracy
{

constexpr float MinVisSize = 3;
constexpr float MinCtxSize = 4;


TimelineItemThread::TimelineItemThread( View& view, Worker& worker, const ThreadData* thread )
    : TimelineItem( view, worker, thread, true )
    , m_thread( thread )
    , m_ghost( false )
{
    auto name = worker.GetThreadName( thread->id );
    if( strncmp( name, "Tracy ", 6 ) == 0 )
    {
        m_showFull = false;
    }
}

bool TimelineItemThread::IsEmpty() const
{
    auto& crash = m_worker.GetCrashEvent();
    return crash.thread != m_thread->id &&
        m_thread->timeline.empty() &&
        m_thread->messages.empty() &&
        m_thread->ghostZones.empty();
}

uint32_t TimelineItemThread::HeaderColor() const
{
    auto& crash = m_worker.GetCrashEvent();
    if( crash.thread == m_thread->id ) return 0xFF2222FF;
    if( m_thread->isFiber ) return 0xFF88FF88;
    return 0xFFFFFFFF;
}

uint32_t TimelineItemThread::HeaderColorInactive() const
{
    auto& crash = m_worker.GetCrashEvent();
    if( crash.thread == m_thread->id ) return 0xFF111188;
    if( m_thread->isFiber ) return 0xFF448844;
    return 0xFF888888;
}

uint32_t TimelineItemThread::HeaderLineColor() const
{
    return 0x33FFFFFF;
}

const char* TimelineItemThread::HeaderLabel() const
{
    return m_worker.GetThreadName( m_thread->id );
}

int64_t TimelineItemThread::RangeBegin() const
{
    int64_t first = std::numeric_limits<int64_t>::max();
    const auto ctx = m_worker.GetContextSwitchData( m_thread->id );
    if( ctx && !ctx->v.empty() )
    {
        first = ctx->v.begin()->Start();
    }
    if( !m_thread->timeline.empty() )
    {
        if( m_thread->timeline.is_magic() )
        {
            auto& tl = *((Vector<ZoneEvent>*)&m_thread->timeline);
            first = std::min( first, tl.front().Start() );
        }
        else
        {
            first = std::min( first, m_thread->timeline.front()->Start() );
        }
    }
    if( !m_thread->messages.empty() )
    {
        first = std::min( first, m_thread->messages.front()->time );
    }
    for( const auto& lock : m_worker.GetLockMap() )
    {
        const auto& lockmap = *lock.second;
        if( !lockmap.valid ) continue;
        auto it = lockmap.threadMap.find( m_thread->id );
        if( it == lockmap.threadMap.end() ) continue;
        const auto thread = it->second;
        auto lptr = lockmap.timeline.data();
        while( lptr->ptr->thread != thread ) lptr++;
        if( lptr->ptr->Time() < first ) first = lptr->ptr->Time();
    }
    return first;
}

int64_t TimelineItemThread::RangeEnd() const
{
    int64_t last = -1;
    const auto ctx = m_worker.GetContextSwitchData( m_thread->id );
    if( ctx && !ctx->v.empty() )
    {
        const auto& back = ctx->v.back();
        last = back.IsEndValid() ? back.End() : back.Start();
    }
    if( !m_thread->timeline.empty() )
    {
        if( m_thread->timeline.is_magic() )
        {
            auto& tl = *((Vector<ZoneEvent>*)&m_thread->timeline);
            last = std::max( last, m_worker.GetZoneEnd( tl.back() ) );
        }
        else
        {
            last = std::max( last, m_worker.GetZoneEnd( *m_thread->timeline.back() ) );
        }
    }
    if( !m_thread->messages.empty() )
    {
        last = std::max( last, m_thread->messages.back()->time );
    }
    for( const auto& lock : m_worker.GetLockMap() )
    {
        const auto& lockmap = *lock.second;
        if( !lockmap.valid ) continue;
        auto it = lockmap.threadMap.find( m_thread->id );
        if( it == lockmap.threadMap.end() ) continue;
        const auto thread = it->second;
        auto eptr = lockmap.timeline.data() + lockmap.timeline.size() - 1;
        while( eptr->ptr->thread != thread ) eptr--;
        if( eptr->ptr->Time() > last ) last = eptr->ptr->Time();
    }
    return last;
}

void TimelineItemThread::HeaderTooltip( const char* label ) const
{
    m_view.HighlightThread( m_thread->id );

    ImGui::BeginTooltip();
    SmallColorBox( GetThreadColor( m_thread->id, 0, m_view.GetViewData().dynamicColors ) );
    ImGui::SameLine();
    ImGui::TextUnformatted( m_worker.GetThreadName( m_thread->id ) );
    ImGui::SameLine();
    ImGui::TextDisabled( "(%s)", RealToString( m_thread->id ) );
    auto& crash = m_worker.GetCrashEvent();
    if( crash.thread == m_thread->id )
    {
        ImGui::SameLine();
        TextColoredUnformatted( ImVec4( 1.f, 0.2f, 0.2f, 1.f ), ICON_FA_SKULL " Crashed" );
    }
    if( m_thread->isFiber )
    {
        ImGui::SameLine();
        TextColoredUnformatted( ImVec4( 0.2f, 0.6f, 0.2f, 1.f ), "Fiber" );
    }

    const auto ctx = m_worker.GetContextSwitchData( m_thread->id );
    const auto first = RangeBegin();
    const auto last = RangeEnd();

    ImGui::Separator();

    size_t lockCnt = 0;
    for( const auto& lock : m_worker.GetLockMap() )
    {
        const auto& lockmap = *lock.second;
        if( !lockmap.valid ) continue;
        auto it = lockmap.threadMap.find( m_thread->id );
        if( it == lockmap.threadMap.end() ) continue;
        lockCnt++;
    }

    if( last >= 0 )
    {
        const auto lifetime = last - first;
        const auto traceLen = m_worker.GetLastTime() - m_worker.GetFirstTime();

        TextFocused( "Appeared at", TimeToString( first ) );
        TextFocused( "Last event at", TimeToString( last ) );
        TextFocused( "Lifetime:", TimeToString( lifetime ) );
        ImGui::SameLine();
        char buf[64];
        PrintStringPercent( buf, lifetime / double( traceLen ) * 100 );
        TextDisabledUnformatted( buf );

        if( ctx )
        {
            TextFocused( "Time in running state:", TimeToString( ctx->runningTime ) );
            ImGui::SameLine();
            PrintStringPercent( buf, ctx->runningTime / double( lifetime ) * 100 );
            TextDisabledUnformatted( buf );
        }
    }

    ImGui::Separator();
    if( !m_thread->timeline.empty() )
    {
        TextFocused( "Zone count:", RealToString( m_thread->count ) );
        TextFocused( "Top-level zones:", RealToString( m_thread->timeline.size() ) );
    }
    if( !m_thread->messages.empty() )
    {
        TextFocused( "Messages:", RealToString( m_thread->messages.size() ) );
    }
    if( lockCnt != 0 )
    {
        TextFocused( "Locks:", RealToString( lockCnt ) );
    }
    if( ctx )
    {
        TextFocused( "Running state regions:", RealToString( ctx->v.size() ) );
    }
    if( !m_thread->samples.empty() )
    {
        TextFocused( "Call stack samples:", RealToString( m_thread->samples.size() ) );
        if( m_thread->kernelSampleCnt != 0 )
        {
            TextFocused( "Kernel samples:", RealToString( m_thread->kernelSampleCnt ) );
            ImGui::SameLine();
            ImGui::TextDisabled( "(%.2f%%)", 100.f * m_thread->kernelSampleCnt / m_thread->samples.size() );
        }
    }
    ImGui::EndTooltip();
}

void TimelineItemThread::HeaderExtraContents( const TimelineContext& ctx, int offset, float labelWidth )
{
    m_view.DrawThreadMessages( ctx, *m_thread, offset );

#ifndef TRACY_NO_STATISTICS
    const bool hasGhostZones = m_worker.AreGhostZonesReady() && !m_thread->ghostZones.empty();
    if( hasGhostZones && !m_thread->timeline.empty() )
    {
        auto draw = ImGui::GetWindowDrawList();
        const auto ty = ImGui::GetTextLineHeight();

        const auto color = m_ghost ? 0xFFAA9999 : 0x88AA7777;
        draw->AddText( ctx.wpos + ImVec2( 1.5f * ty + labelWidth, offset ), color, ICON_FA_GHOST );
        float ghostSz = ImGui::CalcTextSize( ICON_FA_GHOST ).x;

        if( ctx.hover && ImGui::IsMouseHoveringRect( ctx.wpos + ImVec2( 1.5f * ty + labelWidth, offset ), ctx.wpos + ImVec2( 1.5f * ty + labelWidth + ghostSz, offset + ty ) ) )
        {
            if( IsMouseClicked( 0 ) )
            {
                m_ghost = !m_ghost;
            }
        }
    }
#endif
}

bool TimelineItemThread::DrawContents( const TimelineContext& ctx, int& offset )
{
    const auto res = m_view.DrawThread( ctx, *m_thread, m_draw, m_ctxDraw, m_samplesDraw, offset, m_depth );
    if( !res )
    {
        auto& crash = m_worker.GetCrashEvent();
        return crash.thread == m_thread->id;
    }
    return true;
}

void TimelineItemThread::DrawOverlay( const ImVec2& ul, const ImVec2& dr )
{
    m_view.DrawThreadOverlays( *m_thread, ul, dr );
}

void TimelineItemThread::DrawFinished()
{
    m_samplesDraw.clear();
    m_ctxDraw.clear();
    m_draw.clear();
}

void TimelineItemThread::Preprocess( const TimelineContext& ctx, TaskDispatch& td )
{
    assert( m_samplesDraw.empty() );
    assert( m_ctxDraw.empty() );
    assert( m_draw.empty() );

    td.Queue( [this, &ctx] {
#ifndef TRACY_NO_STATISTICS
        if( m_worker.AreGhostZonesReady() && ( m_ghost || ( m_view.GetViewData().ghostZones && m_thread->timeline.empty() ) ) )
        {
            m_depth = PreprocessGhostLevel( ctx, m_thread->ghostZones, 0 );
        }
        else
#endif
        {
            m_depth = PreprocessZoneLevel( ctx, m_thread->timeline, 0 );
        }
    } );

    const auto& vd = m_view.GetViewData();

    if( vd.drawContextSwitches )
    {
        auto ctxSwitch = m_worker.GetContextSwitchData( m_thread->id );
        if( ctxSwitch )
        {
            td.Queue( [this, &ctx, ctxSwitch] {
                PreprocessContextSwitches( ctx, *ctxSwitch );
            } );
        }
    }

    if( vd.drawSamples && !m_thread->samples.empty() )
    {
        td.Queue( [this, &ctx] {
            PreprocessSamples( ctx, m_thread->samples );
        } );
    }
}

#ifndef TRACY_NO_STATISTICS
int TimelineItemThread::PreprocessGhostLevel( const TimelineContext& ctx, const Vector<GhostZone>& vec, int depth )
{
    const auto nspx = ctx.nspx;
    const auto vStart = ctx.vStart;
    const auto vEnd = ctx.vEnd;

    const auto MinVisNs = int64_t( round( GetScale() * MinVisSize * nspx ) );

    auto it = std::lower_bound( vec.begin(), vec.end(), std::max<int64_t>( 0, vStart - 2 * MinVisNs ), [] ( const auto& l, const auto& r ) { return l.end.Val() < r; } );
    if( it == vec.end() ) return depth;

    const auto zitend = std::lower_bound( it, vec.end(), vEnd, [] ( const auto& l, const auto& r ) { return l.start.Val() < r; } );
    if( it == zitend ) return depth;
    if( (zitend-1)->end.Val() < vStart ) return depth;

    int maxdepth = depth + 1;

    while( it < zitend )
    {
        auto& ev = *it;
        const auto end = ev.end.Val();
        const auto zsz = end - ev.start.Val();
        if( zsz < MinVisNs )
        {
            auto nextTime = end + MinVisNs;
            auto next = it + 1;
            for(;;)
            {
                next = std::lower_bound( next, zitend, nextTime, [] ( const auto& l, const auto& r ) { return l.end.Val() < r; } );
                if( next == zitend ) break;
                auto prev = next - 1;
                if( prev == it ) break;
                const auto pt = prev->end.Val();
                const auto nt = next->end.Val();
                if( nt - pt >= MinVisNs ) break;
                nextTime = nt + MinVisNs;
            }
            m_draw.emplace_back( TimelineDraw { TimelineDrawType::GhostFolded, uint16_t( depth ), (void**)&ev, (next-1)->end } );
            it = next;
        }
        else
        {
            if( ev.child >= 0 )
            {
                const auto d = PreprocessGhostLevel( ctx, m_worker.GetGhostChildren( ev.child ), depth + 1 );
                if( d > maxdepth ) maxdepth = d;
            }
            m_draw.emplace_back( TimelineDraw { TimelineDrawType::Ghost, uint16_t( depth ), (void**)&ev } );
            ++it;
        }
    }

    return maxdepth;
}
#endif

int TimelineItemThread::PreprocessZoneLevel( const TimelineContext& ctx, const Vector<short_ptr<ZoneEvent>>& vec, int depth )
{
    if( vec.is_magic() )
    {
        return PreprocessZoneLevel<VectorAdapterDirect<ZoneEvent>>( ctx, *(Vector<ZoneEvent>*)( &vec ), depth );
    }
    else
    {
        return PreprocessZoneLevel<VectorAdapterPointer<ZoneEvent>>( ctx, vec, depth );
    }
}

template<typename Adapter, typename V>
int TimelineItemThread::PreprocessZoneLevel( const TimelineContext& ctx, const V& vec, int depth )
{
    const auto delay = m_worker.GetDelay();
    const auto resolution = m_worker.GetResolution();
    const auto vStart = ctx.vStart;
    const auto vEnd = ctx.vEnd;
    const auto nspx = ctx.nspx;

    const auto MinVisNs = int64_t( round( GetScale() * MinVisSize * nspx ) );

    // cast to uint64_t, so that unended zones (end = -1) are still drawn
    auto it = std::lower_bound( vec.begin(), vec.end(), std::max<int64_t>( 0, vStart - std::max<int64_t>( delay, 2 * MinVisNs ) ), [] ( const auto& l, const auto& r ) { Adapter a; return (uint64_t)a(l).End() < (uint64_t)r; } );
    if( it == vec.end() ) return depth;

    const auto zitend = std::lower_bound( it, vec.end(), vEnd + resolution, [] ( const auto& l, const auto& r ) { Adapter a; return a(l).Start() < r; } );
    if( it == zitend ) return depth;
    Adapter a;
    if( !a(*it).IsEndValid() && m_worker.GetZoneEnd( a(*it) ) < vStart ) return depth;
    if( m_worker.GetZoneEnd( a(*(zitend-1)) ) < vStart ) return depth;

    int maxdepth = depth + 1;

    while( it < zitend )
    {
        auto& ev = a(*it);
        const auto end = m_worker.GetZoneEnd( ev );
        const auto zsz = end - ev.Start();
        if( zsz < MinVisNs )
        {
            auto nextTime = end + MinVisNs;
            auto next = it + 1;
            for(;;)
            {
                next = std::lower_bound( next, zitend, nextTime, [] ( const auto& l, const auto& r ) { Adapter a; return (uint64_t)a(l).End() < (uint64_t)r; } );
                if( next == zitend ) break;
                auto prev = next - 1;
                if( prev == it ) break;
                const auto pt = m_worker.GetZoneEnd( a(*prev) );
                const auto nt = m_worker.GetZoneEnd( a(*next) );
                if( nt - pt >= MinVisNs ) break;
                nextTime = nt + MinVisNs;
            }
            m_draw.emplace_back( TimelineDraw { TimelineDrawType::Folded, uint16_t( depth ), (void**)&ev, m_worker.GetZoneEnd( a(*(next-1)) ), uint32_t( next - it ) } );
            it = next;
        }
        else
        {
            if( ev.HasChildren() )
            {
                const auto d = PreprocessZoneLevel( ctx, m_worker.GetZoneChildren( ev.Child() ), depth + 1 );
                if( d > maxdepth ) maxdepth = d;
            }
            m_draw.emplace_back( TimelineDraw { TimelineDrawType::Zone, uint16_t( depth ), (void**)&ev } );
            ++it;
        }
    }

    return maxdepth;
}

void TimelineItemThread::PreprocessContextSwitches( const TimelineContext& ctx, const ContextSwitch& ctxSwitch )
{
    const auto w = ctx.w;
    const auto pxns = ctx.pxns;
    const auto nspx = ctx.nspx;
    const auto vStart = ctx.vStart;
    const auto vEnd = ctx.vEnd;

    auto& vec = ctxSwitch.v;
    auto it = std::lower_bound( vec.begin(), vec.end(), std::max<int64_t>( 0, vStart ), [] ( const auto& l, const auto& r ) { return (uint64_t)l.End() < (uint64_t)r; } );
    if( it == vec.end() ) return;
    if( it != vec.begin() ) --it;

    auto citend = std::lower_bound( it, vec.end(), vEnd, [] ( const auto& l, const auto& r ) { return l.Start() < r; } );
    if( it == citend ) return;
    if( citend != vec.end() ) ++citend;

    const auto MinCtxNs = MinCtxSize * nspx;
    const auto& sampleData = m_thread->samples;

    auto pit = citend;
    double minpx = -10.0;
    while( it < citend )
    {
        auto& ev = *it;
        if( pit != citend )
        {
            uint32_t waitStack = 0;
            if( !sampleData.empty() )
            {
                auto sdit = std::lower_bound( sampleData.begin(), sampleData.end(), ev.Start(), [] ( const auto& l, const auto& r ) { return l.time.Val() < r; } );
                bool found = sdit != sampleData.end() && sdit->time.Val() == ev.Start();
                if( !found && it != vec.begin() )
                {
                    auto eit = it;
                    --eit;
                    sdit = std::lower_bound( sampleData.begin(), sampleData.end(), eit->End(), [] ( const auto& l, const auto& r ) { return l.time.Val() < r; } );
                    found = sdit != sampleData.end() && sdit->time.Val() == eit->End();
                }
                if( found ) waitStack = sdit->callstack.Val();
            }

            auto& ref = m_ctxDraw.emplace_back( ContextSwitchDraw { ContextSwitchDrawType::Waiting, &ev, float( minpx ) } );
            ref.waiting.prev = pit;
            ref.waiting.waitStack = waitStack;
        }

        const auto end = ev.IsEndValid() ? ev.End() : m_worker.GetLastTime();
        const auto zsz = std::max( ( end - ev.Start() ) * pxns, pxns * 0.5 );
        if( zsz < MinCtxSize )
        {
            int num = 0;
            const auto px0 = std::max( ( ev.Start() - vStart ) * pxns, -10.0 );
            auto px1ns = end - vStart;
            auto rend = end;
            auto nextTime = end + MinCtxNs;
            for(;;)
            {
                const auto prevIt = it;
                it = std::lower_bound( it, citend, nextTime, [] ( const auto& l, const auto& r ) { return (uint64_t)l.End() < (uint64_t)r; } );
                if( it == prevIt ) ++it;
                num += std::distance( prevIt, it );
                if( it == citend ) break;
                const auto nend = it->IsEndValid() ? it->End() : m_worker.GetLastTime();
                const auto nsnext = nend - vStart;
                if( nsnext - px1ns >= MinCtxNs * 2 ) break;
                px1ns = nsnext;
                rend = nend;
                nextTime = nend + nspx;
            }
            minpx = std::min( std::max( px1ns * pxns, px0+MinCtxSize ), double( w + 10 ) );
            if( num == 1 )
            {
                auto& ref = m_ctxDraw.emplace_back( ContextSwitchDraw { ContextSwitchDrawType::FoldedOne, &ev, float( minpx ) } );
                ref.folded.rend = rend;
            }
            else
            {
                auto& ref = m_ctxDraw.emplace_back( ContextSwitchDraw { ContextSwitchDrawType::FoldedMulti, &ev, float( minpx ) } );
                ref.folded.rend = rend;
                ref.folded.num = num;
            }
            pit = it-1;
        }
        else
        {
            m_ctxDraw.emplace_back( ContextSwitchDraw { ContextSwitchDrawType::Running, &ev, float( minpx ) } );
            pit = it;
            ++it;
        }
    }
}

void TimelineItemThread::PreprocessSamples( const TimelineContext& ctx, const Vector<SampleData>& vec )
{
    const auto vStart = ctx.vStart;
    const auto vEnd = ctx.vEnd;
    const auto nspx = ctx.nspx;

    const auto MinVis = 5 * GetScale();
    const auto MinVisNs = int64_t( round( MinVis * nspx ) );

    auto it = std::lower_bound( vec.begin(), vec.end(), vStart - MinVisNs, [] ( const auto& l, const auto& r ) { return l.time.Val() < r; } );
    if( it == vec.end() ) return;
    const auto itend = std::lower_bound( it, vec.end(), vEnd, [] ( const auto& l, const auto& r ) { return l.time.Val() < r; } );
    if( it == itend ) return;

    while( it < itend )
    {
        auto next = it + 1;
        if( next != itend )
        {
            const auto t0 = it->time.Val();
            auto nextTime = t0 + MinVisNs;
            for(;;)
            {
                next = std::lower_bound( next, itend, nextTime, [] ( const auto& l, const auto& r ) { return l.time.Val() < r; } );
                if( next == itend ) break;
                auto prev = next - 1;
                if( prev == it ) break;
                const auto pt = prev->time.Val();
                const auto nt = next->time.Val();
                if( nt - pt >= MinVisNs ) break;
                nextTime = nt + MinVisNs;
            }
        }
        m_samplesDraw.emplace_back( SamplesDraw{ uint32_t( next - it - 1 ), uint32_t( it - vec.begin() ) } );
        it = next;
    }
}

}
