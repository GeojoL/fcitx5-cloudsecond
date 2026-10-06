// SPDX-License-Identifier: LGPL-2.1-or-later
// Second cloud candidate for fcitx5 pinyin.
//
// fcitx5's cloudpinyin only keeps the first result of the cloud backend. This
// addon asks Baidu for the same pinyin and puts its second result right after
// the cloud candidate, so the first two candidates both come from the cloud
// and local candidates start from the third.
//
// It never touches the list until the regular cloud candidate has been
// filled: an unfilled cloud candidate in front selects its neighbour, which
// must stay the local best guess.

#include <cloudpinyin_public.h>
#include <curl/curl.h>
#include <fcitx-utils/capabilityflags.h>
#include <fcitx-utils/eventdispatcher.h>
#include <fcitx-utils/trackableobject.h>
#include <fcitx/addonfactory.h>
#include <fcitx/addoninstance.h>
#include <fcitx/addonmanager.h>
#include <fcitx/candidatelist.h>
#include <fcitx/event.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>
#include <fcitx/text.h>
#include <fcitx/userinterface.h>
#include <nlohmann/json.hpp>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

using namespace fcitx;

// Same threshold as cloudpinyin's default MinimumPinyinLength.
constexpr size_t kMinPinyinLength = 4;
constexpr long kTimeoutMs = 2000;
constexpr size_t kCacheSize = 512;

size_t appendBody(char *data, size_t size, size_t count, void *user) {
    static_cast<std::string *>(user)->append(data, size * count);
    return size * count;
}

std::vector<std::string> fetchBaidu(const std::string &pinyin) {
    std::vector<std::string> words;
    CURL *curl = curl_easy_init();
    if (!curl) {
        return words;
    }
    char *escaped =
        curl_easy_escape(curl, pinyin.c_str(), static_cast<int>(pinyin.size()));
    std::string url = "https://olimenew.baidu.com/py?input=" +
                      std::string(escaped ? escaped : "") +
                      "&inputtype=py&resultcoding=utf-8";
    curl_free(escaped);
    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, appendBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, kTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    bool ok = curl_easy_perform(curl) == CURLE_OK;
    curl_easy_cleanup(curl);
    if (!ok) {
        return words;
    }
    try {
        // {"status":"T","result":[[["什么",6,{...}],["神恶名",9,{...}]]]}
        auto json = nlohmann::json::parse(body);
        if (json.at("status") != "T") {
            return words;
        }
        for (const auto &item : json.at("result").at(0)) {
            words.push_back(item.at(0).get<std::string>());
        }
    } catch (const std::exception &) {
        words.clear();
    }
    return words;
}

// One worker thread; a newer request replaces one that has not started yet.
class Fetcher {
public:
    using Callback = std::function<void(std::string, std::vector<std::string>)>;

    Fetcher(EventDispatcher *dispatcher, Callback callback)
        : dispatcher_(dispatcher), callback_(std::move(callback)),
          thread_([this] { run(); }) {}

    ~Fetcher() {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
        }
        cv_.notify_one();
        thread_.join();
    }

    void request(const std::string &pinyin) {
        {
            std::lock_guard lock(mutex_);
            if (pinyin == running_ || pinyin == pending_) {
                return;
            }
            pending_ = pinyin;
        }
        cv_.notify_one();
    }

private:
    void run() {
        std::unique_lock lock(mutex_);
        while (true) {
            cv_.wait(lock, [this] { return stop_ || !pending_.empty(); });
            if (stop_) {
                return;
            }
            running_ = std::move(pending_);
            pending_.clear();
            lock.unlock();
            auto words = fetchBaidu(running_);
            dispatcher_->schedule(
                [this, pinyin = running_, words = std::move(words)]() mutable {
                    callback_(std::move(pinyin), std::move(words));
                });
            lock.lock();
            running_.clear();
        }
    }

    EventDispatcher *dispatcher_;
    Callback callback_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::string pending_;
    std::string running_;
    bool stop_ = false;
    std::thread thread_;
};

class CloudSecondCandidateWord : public CandidateWord {
public:
    CloudSecondCandidateWord(std::string selected, std::string word)
        : CandidateWord(Text(word)), selected_(std::move(selected)),
          word_(std::move(word)) {}

    void select(InputContext *inputContext) const override {
        inputContext->commitString(selected_ + word_);
        // Let the input method drop its composing state.
        inputContext->reset();
    }

private:
    std::string selected_;
    std::string word_;
};

// Pinyin shows "<selected hanzi><pinyin segments separated by spaces>".
std::optional<std::pair<std::string, std::string>>
splitPreedit(const std::string &preedit) {
    auto start = preedit.find_first_of("abcdefghijklmnopqrstuvwxyz'");
    if (start == std::string::npos) {
        return std::nullopt;
    }
    std::string pinyin;
    for (char c : preedit.substr(start)) {
        if (c == ' ') {
            continue;
        }
        if ((c < 'a' || c > 'z') && c != '\'') {
            return std::nullopt;
        }
        pinyin.push_back(c);
    }
    return std::make_pair(preedit.substr(0, start), pinyin);
}

class CloudSecond : public AddonInstance {
public:
    explicit CloudSecond(Instance *instance) : instance_(instance) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        dispatcher_.attach(&instance_->eventLoop());
        fetcher_ = std::make_unique<Fetcher>(
            &dispatcher_,
            [this](std::string pinyin, std::vector<std::string> words) {
                onFetched(std::move(pinyin), std::move(words));
            });
        handler_ = instance_->watchEvent(
            EventType::InputContextUpdateUI, EventWatcherPhase::PostInputMethod,
            [this](Event &event) {
                auto &uiEvent = static_cast<InputContextUpdateUIEvent &>(event);
                if (uiEvent.component() == UserInterfaceComponent::InputPanel) {
                    onUpdate(uiEvent.inputContext());
                }
            });
    }

    ~CloudSecond() override {
        // Join the worker before the dispatcher goes away.
        fetcher_.reset();
        dispatcher_.detach();
    }

private:
    void onUpdate(InputContext *ic) {
        auto list = ic->inputPanel().candidateList();
        if (!list || !list->toModifiable() || handled_.lock() == list) {
            return;
        }
        if (ic->capabilityFlags().testAny(CapabilityFlag::PasswordOrSensitive) ||
            instance_->inputMethod(ic) != "pinyin") {
            return;
        }
        auto preedit = ic->inputPanel().preedit().toString();
        if (preedit.empty()) {
            preedit = ic->inputPanel().clientPreedit().toString();
        }
        auto parts = splitPreedit(preedit);
        if (!parts || parts->second.size() < kMinPinyinLength) {
            return;
        }
        auto &[selected, pinyin] = *parts;
        if (auto iter = cache_.find(pinyin); iter != cache_.end()) {
            apply(list, selected, iter->second);
            return;
        }
        pendingIc_ = ic->watch();
        pendingList_ = list;
        pendingSelected_ = selected;
        pendingPinyin_ = pinyin;
        fetcher_->request(pinyin);
    }

    void onFetched(std::string pinyin, std::vector<std::string> words) {
        if (words.empty()) {
            // Network error or no result; let a later update retry.
        } else {
            if (cache_.size() >= kCacheSize) {
                cache_.clear();
            }
            cache_[pinyin] = words;
        }
        auto *ic = pendingIc_.get();
        auto list = pendingList_.lock();
        if (!ic || !list || pinyin != pendingPinyin_ ||
            ic->inputPanel().candidateList() != list || words.empty()) {
            return;
        }
        if (apply(list, pendingSelected_, words)) {
            ic->updateUserInterface(UserInterfaceComponent::InputPanel);
        }
    }

    // Returns true if the list was changed.
    bool apply(const std::shared_ptr<CandidateList> &list,
               const std::string &selected,
               const std::vector<std::string> &words) {
        auto *modifiable = list->toModifiable();
        const int total = modifiable->totalSize();
        int insertAt = -1;
        for (int i = 0; i < total; i++) {
            const auto *cloud = dynamic_cast<const CloudPinyinCandidateWord *>(
                &modifiable->candidateFromAll(i));
            if (!cloud) {
                continue;
            }
            if (!cloud->filled()) {
                // Wait: its fill triggers another UI update.
                return false;
            }
            if (!cloud->word().empty()) {
                insertAt = i + 1;
            }
            break;
        }
        // cloudpinyin moves a local duplicate of its word to the front and
        // removes itself.
        if (insertAt < 0 && total > 0 && !words.empty() &&
            modifiable->candidateFromAll(0).text().toString() == words[0]) {
            insertAt = 1;
        }
        handled_ = list;
        if (insertAt < 0 || words.size() < 2) {
            return false;
        }
        const auto &word = words[1];
        for (int i = 0; i < total; i++) {
            if (modifiable->candidateFromAll(i).text().toString() == word) {
                if (i <= insertAt) {
                    return false;
                }
                // Prefer the input method's own candidate, like cloudpinyin.
                modifiable->move(i, insertAt);
                return true;
            }
        }
        modifiable->insert(
            insertAt, std::make_unique<CloudSecondCandidateWord>(selected, word));
        return true;
    }

    Instance *instance_;
    EventDispatcher dispatcher_;
    std::unique_ptr<Fetcher> fetcher_;
    std::unique_ptr<HandlerTableEntry<EventHandler>> handler_;
    std::unordered_map<std::string, std::vector<std::string>> cache_;
    std::weak_ptr<CandidateList> handled_;
    TrackableObjectReference<InputContext> pendingIc_;
    std::weak_ptr<CandidateList> pendingList_;
    std::string pendingSelected_;
    std::string pendingPinyin_;
};

class CloudSecondFactory : public AddonFactory {
public:
    AddonInstance *create(AddonManager *manager) override {
        return new CloudSecond(manager->instance());
    }
};

} // namespace

FCITX_ADDON_FACTORY_V2_BACKWARDS(cloudsecond, CloudSecondFactory)
