#pragma once

#include <cassert>

class AudioCallbackContext {
public:
    class Scope {
    public:
        Scope()
            : previous_(in_audio_callback_) {
            in_audio_callback_ = true;
        }

        ~Scope() {
            in_audio_callback_ = previous_;
        }

    private:
        bool previous_;
    };

    static void assert_not_reclamation() {
        assert(!in_audio_callback_);
    }

private:
    inline static thread_local bool in_audio_callback_ = false;
};
