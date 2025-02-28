/*
 * Copyright (c) 2022, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/Function.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Queue.h>
#include <AK/Time.h>
#include <LibCore/EventLoop.h>
#include <LibCore/SharedCircularQueue.h>
#include <LibGfx/Bitmap.h>
#include <LibThreading/ConditionVariable.h>
#include <LibThreading/Mutex.h>
#include <LibThreading/Thread.h>
#include <LibVideo/Containers/Demuxer.h>
#include <LibVideo/Containers/Matroska/Document.h>

#include "VideoDecoder.h"

namespace Video {

struct FrameQueueItem {
    enum class Type {
        Frame,
        Error,
    };

    static FrameQueueItem frame(RefPtr<Gfx::Bitmap> bitmap, Time timestamp)
    {
        return FrameQueueItem(move(bitmap), timestamp);
    }

    static FrameQueueItem error_marker(DecoderError&& error)
    {
        return FrameQueueItem(move(error));
    }

    bool is_frame() const { return m_data.has<FrameData>(); }
    RefPtr<Gfx::Bitmap> bitmap() const { return m_data.get<FrameData>().bitmap; }
    Time timestamp() const { return m_data.get<FrameData>().timestamp; }

    bool is_error() const { return m_data.has<DecoderError>(); }
    DecoderError const& error() const { return m_data.get<DecoderError>(); }
    DecoderError release_error()
    {
        auto error = move(m_data.get<DecoderError>());
        m_data.set(Empty());
        return error;
    }

    DeprecatedString debug_string() const
    {
        if (is_error())
            return error().string_literal();
        return DeprecatedString::formatted("frame at {}ms", timestamp().to_milliseconds());
    }

private:
    struct FrameData {
        RefPtr<Gfx::Bitmap> bitmap;
        Time timestamp;
    };

    FrameQueueItem(RefPtr<Gfx::Bitmap> bitmap, Time timestamp)
        : m_data(FrameData { move(bitmap), timestamp })
    {
    }

    FrameQueueItem(DecoderError&& error)
        : m_data(move(error))
    {
    }

    Variant<Empty, FrameData, DecoderError> m_data;
};

static constexpr size_t FRAME_BUFFER_COUNT = 4;
using VideoFrameQueue = Queue<FrameQueueItem, FRAME_BUFFER_COUNT>;

class PlaybackManager {
public:
    enum class SeekMode {
        Accurate,
        Fast,
    };

    static constexpr SeekMode DEFAULT_SEEK_MODE = SeekMode::Accurate;

    static DecoderErrorOr<NonnullOwnPtr<PlaybackManager>> from_file(Core::Object& event_handler, StringView file);

    PlaybackManager(Core::Object& event_handler, NonnullOwnPtr<Demuxer>& demuxer, Track video_track, NonnullOwnPtr<VideoDecoder>&& decoder);

    void resume_playback();
    void pause_playback();
    void restart_playback();
    void seek_to_timestamp(Time, SeekMode = DEFAULT_SEEK_MODE);
    bool is_playing() const
    {
        return m_playback_handler->is_playing();
    }

    u64 number_of_skipped_frames() const { return m_skipped_frames; }

    void on_decoder_error(DecoderError error);

    Time current_playback_time();
    Time duration();

    Function<void(NonnullRefPtr<Gfx::Bitmap>, Time)> on_frame_present;

private:
    class PlaybackStateHandler;
    class PlayingStateHandler;
    class PausedStateHandler;
    class BufferingStateHandler;
    class SeekingStateHandler;
    class StoppedStateHandler;

    void start_timer(int milliseconds);
    void timer_callback();
    Optional<Time> seek_demuxer_to_most_recent_keyframe(Time timestamp, Optional<Time> earliest_available_sample = OptionalNone());

    bool decode_and_queue_one_sample();
    void on_decode_timer();

    void dispatch_decoder_error(DecoderError error);
    void dispatch_new_frame(RefPtr<Gfx::Bitmap> frame);
    void dispatch_fatal_error(Error);

    Core::Object& m_event_handler;
    Core::EventLoop& m_main_loop;

    Time m_last_present_in_media_time = Time::zero();

    NonnullOwnPtr<Demuxer> m_demuxer;
    Track m_selected_video_track;
    NonnullOwnPtr<VideoDecoder> m_decoder;

    NonnullOwnPtr<VideoFrameQueue> m_frame_queue;

    RefPtr<Core::Timer> m_present_timer;
    unsigned m_decoding_buffer_time_ms = 16;

    RefPtr<Core::Timer> m_decode_timer;

    NonnullOwnPtr<PlaybackStateHandler> m_playback_handler;
    Optional<FrameQueueItem> m_next_frame;

    u64 m_skipped_frames;

    // This is a nested class to allow private access.
    class PlaybackStateHandler {
    public:
        PlaybackStateHandler(PlaybackManager& manager)
            : m_manager(manager)
        {
        }
        virtual ~PlaybackStateHandler() = default;
        virtual StringView name() = 0;

        virtual ErrorOr<void> on_enter() { return {}; }

        virtual ErrorOr<void> play() { return {}; };
        virtual bool is_playing() = 0;
        virtual ErrorOr<void> pause() { return {}; };
        virtual ErrorOr<void> buffer() { return {}; };
        virtual ErrorOr<void> seek(Time target_timestamp, SeekMode);
        virtual ErrorOr<void> stop();

        virtual Time current_time() const;

        virtual ErrorOr<void> on_timer_callback() { return {}; };
        virtual ErrorOr<void> on_buffer_filled() { return {}; };

    protected:
        template<class T, class... Args>
        ErrorOr<void> replace_handler_and_delete_this(Args... args);

        PlaybackManager& manager() const;

        PlaybackManager& manager()
        {
            return const_cast<PlaybackManager&>(const_cast<PlaybackStateHandler const*>(this)->manager());
        }

    private:
        PlaybackManager& m_manager;
#if PLAYBACK_MANAGER_DEBUG
        bool m_has_exited { false };
#endif
    };
};

enum EventType : unsigned {
    DecoderErrorOccurred = (('v' << 2) | ('i' << 1) | 'd') << 4,
    VideoFramePresent,
    PlaybackStateChange,
    FatalPlaybackError,
};

class DecoderErrorEvent : public Core::Event {
public:
    explicit DecoderErrorEvent(DecoderError error)
        : Core::Event(DecoderErrorOccurred)
        , m_error(move(error))
    {
    }
    virtual ~DecoderErrorEvent() = default;

    DecoderError const& error() { return m_error; }

private:
    DecoderError m_error;
};

class VideoFramePresentEvent : public Core::Event {
public:
    VideoFramePresentEvent() = default;
    explicit VideoFramePresentEvent(RefPtr<Gfx::Bitmap> frame)
        : Core::Event(VideoFramePresent)
        , m_frame(move(frame))
    {
    }
    virtual ~VideoFramePresentEvent() = default;

    RefPtr<Gfx::Bitmap> frame() { return m_frame; }

private:
    RefPtr<Gfx::Bitmap> m_frame;
};

class PlaybackStateChangeEvent : public Core::Event {
public:
    explicit PlaybackStateChangeEvent()
        : Core::Event(PlaybackStateChange)
    {
    }
    virtual ~PlaybackStateChangeEvent() = default;
};

class FatalPlaybackErrorEvent : public Core::Event {
public:
    explicit FatalPlaybackErrorEvent(Error error)
        : Core::Event(FatalPlaybackError)
        , m_error(error)
    {
    }
    virtual ~FatalPlaybackErrorEvent() = default;
    Error error() { return m_error; }

private:
    Error m_error;
};

}
