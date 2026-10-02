// Shared by every screen: the one object a view needs to reach the data and
// the app. Passed by reference into screen constructors; owned by main().
//
// Contract between the shell (app/screens/shell: window, rail, sidebar,
// header, composer, main) and the messages screens (app/screens/messages:
// message list, message rows, thread panel, image cache). Additive changes are
// fine; changing a member needs the lead.
#pragma once

#include "model/backend.h"
#include "model/store.h"
#include "model/store_slot.h"
#include "ui/ui.h"

#include <functional>
#include <string>
#include <vector>

namespace llm {
class Service; // app/llm/service.h
}
namespace media {
class AudioPlayer; // app/media/audio_player.h
}

namespace screens {

// The model types every screen names (Context uses them unqualified).
using model::Backend;
using model::ConvRef;
using model::Store;
using model::Ts;
using model::UserRef;

class ImageCache;   // app/screens/messages/image_cache.h (decoded, scaled, LRU-bounded)
class RemoteImages; // app/screens/common/remote_images.h (downloads + their disk cache)

struct Context {
    ui::App         &app;
    // The open workspace's Store (store() reads it). Observe through the
    // slot itself: a workspace switch moves those observers along.
    model::StoreSlot store;
    Backend         &backend;
    ImageCache      &images;

    // Navigation requests any screen may make; the shell implements them.
    std::function<void(ConvRef)>                   openConversation;
    std::function<void(ConvRef, Ts root)>          openThread; // shows the thread panel
    std::function<void()>                          closeThread;
    std::function<void(UserRef)>                   openProfile; // may be a no-op for now
    std::function<void(const std::string &)>       openUrl;     // links in messages
    // msga's openMessageTarget (a message link's chip): the conversation (or
    // the thread, for a reply) with the message scrolled to and flashed.
    std::function<void(ConvRef, Ts ts, Ts thread)> openMessage;

    // msga's hover profile card: the pointer entered (1) or left (0) an
    // avatar, name or mention standing for the user, or clicked it (2);
    // the rect is its window rect.
    std::function<void(UserRef, ui::RectF, int mode)>             profileHover;
    // msga's openDmWith (a profile card's "Message", the member list): the
    // user's DM, opened (or created) and shown — or, for a teammate (an
    // agent's avatar in a Claude Code session, which has no DM), its page.
    std::function<void(UserRef)>                                  messageUser;
    // msga's Forward dialog for a message, or for one of its files (msga's
    // "Forward this file", from the file bar's Share).
    std::function<void(ConvRef, Ts, const std::string &filePath)> forwardMessage;

    // The main window: parent of file dialogs, host of the in-app file
    // browser (screens/common/file_dialogs.h). Set by the shell.
    ui::Window                                                      *window = nullptr;
    // Files dropped on a message list become attachments of the composer
    // below it (thread = the thread panel's). Set by the shell.
    std::function<void(std::vector<std::string> paths, bool thread)> attachFiles;
    // Settings → Link previews (the shell sets it and rebuilds the rows).
    bool                                                             linkPreviews  = true;
    // Settings → Threads "Inline": a thread's replies open under its message
    // in the list instead of the side panel (the shell rebuilds the rows).
    bool                                                             threadsInline = false;
    // A canvas file shared in a message (msga's CanvasViewerOverlay: the
    // canvas page in a popup). Set by the shell; unset = the file's page.
    std::function<void(ConvRef, const model::File &)>                openCanvas;
    // http(s) pictures (avatars, files, emoji, previews): main's; null in
    // tests that only show local files.
    RemoteImages                                                    *remote = nullptr;
    // The AI providers (Settings → AI assistance): main's; null where no AI
    // feature is wired (tests that don't need one).
    llm::Service                                                    *ai     = nullptr;
    // The inline audio player (one clip at a time, app-wide): main's; null
    // where nothing plays (the cards still show, inert).
    media::AudioPlayer                                              *audio  = nullptr;
    // Settings opened on its AI assistance page (the Summarize notice's
    // "Open settings"). Set by the shell.
    std::function<void()>                                            openAiSettings;
};

} // namespace screens
