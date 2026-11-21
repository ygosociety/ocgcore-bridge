// SPDX-License-Identifier: AGPL-3.0-or-later
// The protocol v1 server (server.h): one duel per process, newline-delimited
// JSON over stdin / stdout (docs/protocol.md). Logs go to stderr.
#include "server.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <string>

#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ocgbridge/answers.h"
#include "ocgbridge/events.h"
#include "ocgbridge/session.h"
#include "protocol.h"

using json = nlohmann::json;
using namespace ocgbridge;
using protocol::Error;

namespace {

// Line I/O on raw file descriptors: a forked session gets a fresh socket and a
// fresh buffer (nothing read ahead by the parent leaks into it). A reader of a
// fork's socket also takes the descriptors sent with its lines (a detached
// fork's socket, see do_fork).
class LineReader {
public:
    explicit LineReader(int fd, bool takes_fds = false) : fd_(fd), takes_fds_(takes_fds) {}
    ~LineReader() { for (int f : fds_) close(f); }
    bool next(std::string& line) {
        for (;;) {
            size_t nl = buf_.find('\n', pos_);
            if (nl != std::string::npos) {
                line.assign(buf_, pos_, nl - pos_);
                pos_ = nl + 1;
                if (pos_ > 65536) { buf_.erase(0, pos_); pos_ = 0; }
                if (!line.empty() && line.back() == '\r') line.pop_back();
                return true;
            }
            char chunk[65536];
            ssize_t n = takes_fds_ ? receive(chunk, sizeof chunk) : read(fd_, chunk, sizeof chunk);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                if (pos_ < buf_.size()) { line.assign(buf_, pos_, std::string::npos); pos_ = buf_.size(); return true; }
                return false;
            }
            buf_.append(chunk, static_cast<size_t>(n));
        }
    }
    int fd() const { return fd_; }
    // The oldest descriptor received and not taken yet (-1: none).
    int take_fd() {
        if (fds_.empty()) return -1;
        const int f = fds_.front();
        fds_.pop_front();
        return f;
    }
private:
    ssize_t receive(char* chunk, size_t size) {
        struct iovec iov{chunk, size};
        alignas(struct cmsghdr) char control[CMSG_SPACE(sizeof(int) * 4)];
        struct msghdr mh{};
        mh.msg_iov = &iov;
        mh.msg_iovlen = 1;
        mh.msg_control = control;
        mh.msg_controllen = sizeof control;
        const ssize_t n = recvmsg(fd_, &mh, 0);
        if (n <= 0) return n;
        for (struct cmsghdr* c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
            if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
            const size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            const int* p = reinterpret_cast<const int*>(CMSG_DATA(c));
            for (size_t i = 0; i < count; ++i) fds_.push_back(p[i]);
        }
        return n;
    }
    int fd_;
    bool takes_fds_;
    std::string buf_;
    size_t pos_{0};
    std::deque<int> fds_;
};

bool write_all(int fd, const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
        ssize_t n = write(fd, s.data() + off, s.size() - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

// A line with a descriptor attached (SCM_RIGHTS on a Unix socket).
bool write_with_fd(int fd, const std::string& s, int pass) {
    struct iovec iov{const_cast<char*>(s.data()), s.size()};
    alignas(struct cmsghdr) char control[CMSG_SPACE(sizeof(int))];
    std::memset(control, 0, sizeof control);
    struct msghdr mh{};
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = control;
    mh.msg_controllen = sizeof control;
    struct cmsghdr* c = CMSG_FIRSTHDR(&mh);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(c), &pass, sizeof(int));
    ssize_t n;
    do n = sendmsg(fd, &mh, 0); while (n < 0 && errno == EINTR);
    if (n <= 0) return false;
    return write_all(fd, s.substr(static_cast<size_t>(n)));
}

// A fork's channel: one Unix socket both ways. pid -1: a detached fork made
// by another process (its parent reaps it, see do_fork).
struct Fork {
    pid_t pid{-1};
    int to_child{-1};
    std::unique_ptr<LineReader> from_child;
};

std::vector<std::string> split_list(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        const size_t comma = s.find(',', start);
        out.push_back(s.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
        if (comma == std::string::npos) return out;
        start = comma + 1;
    }
}

class Server {
public:
    explicit Server(const server::Extensions& ext) : ext_(ext), session_(&cards_, "") {}

    int run(int argc, char** argv);

private:
    void send(const json& msg) { write_all(out_fd_, msg.dump() + "\n"); }
    // A reply line; it carries pass_fd_ (a detached fork's socket) if one is waiting.
    void reply_line(const std::string& line);
    bool next_line(std::string& line);
    bool handle(const std::string& line);
    json dispatch(const std::string& type, const json& msg);
    std::string relay(const std::string& session, json msg);
    json do_fork(const json& msg);
    json do_batch(const json& msg);
    void close_fork(const std::string& id);
    void reap(bool wait);
    bool ask(Session& s, const json& prompt, std::vector<uint8_t>& resp);
    long load_cards(const json& msg);
    void install_event_filters(const protocol::InitOptions& o);
    json state() const { return session_.state_json(); }

    const server::Extensions& ext_;
    CardDatabase cards_;
    std::string scripts_;
    Session session_;
    LineReader* in_{nullptr};
    std::unique_ptr<LineReader> own_in_;
    int out_fd_{STDOUT_FILENO};
    std::deque<std::string> queued_;
    bool asks_{true};
    long next_ask_{1};
    long next_fork_{1};
    std::map<std::string, Fork> forks_;
    std::vector<pid_t> exiting_; // closed forks not reaped yet
    int pass_fd_{-1};             // a detached fork's socket, sent with the next reply
    std::vector<int> sent_fds_;   // sockets sent, closed once "received" (or any request) comes
    bool is_fork_{false};
    bool just_forked_{false};
    std::vector<std::string> script_dirs_{default_script_dirs()};
    std::vector<int> protocol_filters_;
};

// init.events: a filter that drops, then one that sends events out as
// { type: "event", event: { msg, name, ...fields, raw } }.
void Server::install_event_filters(const protocol::InitOptions& o) {
    for (int id : protocol_filters_) session_.remove_message_filter(id);
    protocol_filters_.clear();
    if (!o.drop.empty()) {
        const std::set<uint8_t> drop = o.drop;
        protocol_filters_.push_back(session_.add_message_filter([drop](Session&, const EngineMessage& m) {
            return drop.count(m.type) ? FilterResult::Drop : FilterResult::Pass;
        }));
    }
    if (o.events_all || !o.events.empty()) {
        const bool all = o.events_all;
        const std::set<uint8_t> events = o.events;
        protocol_filters_.push_back(session_.add_message_filter([this, all, events](Session&, const EngineMessage& m) {
            if (all || events.count(m.type)) {
                json ev = m.decoded();
                ev["raw"] = to_hex(m.data, m.size);
                send({{"type", "event"}, {"event", std::move(ev)}});
            }
            return FilterResult::Pass;
        }));
    }
}

bool Server::next_line(std::string& line) {
    if (!queued_.empty()) {
        line = std::move(queued_.front());
        queued_.pop_front();
        return true;
    }
    return in_->next(line);
}

// The ask hook: the prompt goes out as { type: "ask" }, then input is read until
// its { type: "answer" } comes back; requests read meanwhile run afterwards.
// A closed input ends the wait with no answer (the default applies).
bool Server::ask(Session& s, const json& prompt, std::vector<uint8_t>& resp) {
    const long id = next_ask_++;
    send({{"type", "ask"}, {"ask", id}, {"prompt", prompt}, {"context", s.context()}});
    std::string line;
    while (in_->next(line)) {
        json msg;
        try { msg = json::parse(line); } catch (...) { queued_.push_back(std::move(line)); continue; }
        if (msg.is_object() && msg.value("type", "") == "answer" && msg.value("ask", 0L) == id) {
            const json answer = msg.value("answer", json());
            if (answer.is_null()) return false;
            std::string reason;
            if (encode_answer(s.pending_buf(), answer, resp, reason)) return true;
            std::fprintf(stderr, "[ocgcore-bridge] ask %ld: answer rejected (%s), default answer used\n", id, reason.c_str());
            return false;
        }
        queued_.push_back(std::move(line));
    }
    return false;
}

long Server::load_cards(const json& msg) {
    long added = 0;
    std::string err;
    if (msg.contains("cdb")) {
        json list = msg["cdb"].is_array() ? msg["cdb"] : json::array({msg["cdb"]});
        for (const auto& p : list) {
            if (!p.is_string()) throw Error("bad_request", "cdb must be a path or an array of paths");
            long n = cards_.load_cdb(p.get<std::string>(), &err);
            if (n < 0) throw Error("cards", err);
            added += n;
        }
    }
    if (msg.contains("json")) {
        if (!msg["json"].is_string()) throw Error("bad_request", "json must be a path");
        JsonSourceOptions opts;
        opts.setcodes_json = msg.value("setcodes", std::string{});
        opts.archetype_constants = msg.value("archetypes", std::string{});
        if (opts.archetype_constants.empty() && !scripts_.empty()) opts.archetype_constants = scripts_ + "/archetype_setcode_constants.lua";
        long n = cards_.load_ygoprodeck_json(msg["json"].get<std::string>(), opts, &err);
        if (n < 0) throw Error("cards", err);
        added += n;
    }
    return added;
}

// fork: a copy of this process. Its channel is a Unix socket pair. With
// `detach` in a fork, the copy is made here but is handed (its socket, passed
// up with the reply) to the process that received the request, which talks to
// it directly: the copy outlives this one, and its requests skip the forks in
// between. Forks ignore SIGCHLD, so a detached copy never waits to be reaped.
json Server::do_fork(const json& msg) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) throw Error("fork", std::strerror(errno));
    const bool child_asks = false; // a fork's prompts take the defaults
    const bool snapshots = msg.value("chainStartSnapshots", false);
    const bool detach = msg.value("detach", false);
    std::fflush(stderr);
    const pid_t pid = fork();
    if (pid < 0) {
        close(sv[0]); close(sv[1]);
        throw Error("fork", std::strerror(errno));
    }
    if (pid == 0) {
        // The copy: the duel, the Lua states and every global, copy-on-write.
        close(sv[0]);
        for (auto& [id, f] : forks_) { close(f.to_child); }
        forks_.clear();
        exiting_.clear(); // the parent's to reap
        if (is_fork_) {
            // This process's own channel belongs to it, not to the copy.
            close(in_->fd());
            if (out_fd_ != in_->fd()) close(out_fd_);
        }
        own_in_ = std::make_unique<LineReader>(sv[1]);
        in_ = own_in_.get();
        out_fd_ = sv[1];
        queued_.clear();
        asks_ = child_asks;
        is_fork_ = true;
        just_forked_ = true;
        next_fork_ = 1;
        struct sigaction sa{};
        sa.sa_handler = SIG_IGN;
        sa.sa_flags = SA_NOCLDWAIT;
        sigaction(SIGCHLD, &sa, nullptr);
        session_.set_chain_start_snapshots(snapshots);
        if (!asks_) session_.set_ask_hook(nullptr);
        return nullptr; // the new fork answers nothing: its parent replied for it
    }
    close(sv[1]);
    if (detach && is_fork_) {
        pass_fd_ = sv[0];
        return {{"detached", true}};
    }
    const std::string id = std::to_string(next_fork_++);
    Fork f;
    f.pid = pid;
    f.to_child = sv[0];
    f.from_child = std::make_unique<LineReader>(sv[0], true);
    forks_.emplace(id, std::move(f));
    json out = {{"session", id}};
    if (detach) out["detached"] = true;
    return out;
}

void Server::reply_line(const std::string& line) {
    if (pass_fd_ < 0) { write_all(out_fd_, line + "\n"); return; }
    write_with_fd(out_fd_, line + "\n", pass_fd_);
    // Kept open until the receiver says it holds it ("received"): closing it
    // while it is in flight lets some kernels' garbage collection of
    // in-flight Unix sockets (macOS) shut its reading side down.
    sent_fds_.push_back(pass_fd_);
    pass_fd_ = -1;
}

// A closed fork sees the end of its input and exits; it is reaped without
// waiting for it (reap), so a destroy does not wait for the process to go.
void Server::close_fork(const std::string& id) {
    auto it = forks_.find(id);
    if (it == forks_.end()) return;
    close(it->second.to_child);
    if (it->second.pid > 0) exiting_.push_back(it->second.pid);
    forks_.erase(it);
    reap(false);
}

void Server::reap(bool wait) {
    for (auto it = exiting_.begin(); it != exiting_.end();) {
        int status = 0;
        const pid_t r = waitpid(*it, &status, wait ? 0 : WNOHANG);
        if (r == 0) { ++it; continue; }
        it = exiting_.erase(it);
    }
}

// A request for a fork ("3", or "3/1" for a fork of fork 3): sent on to it, and
// its one reply line comes back. The reply line is passed on as it is (the
// fork already echoes the request's id); only a fork's own `fork` reply is
// rewritten, so that its session is addressed through this process. A
// detached fork's reply brings its socket: a fork passes it on up, the main
// process adopts it as one of its own sessions.
std::string Server::relay(const std::string& session, json msg) {
    const size_t slash = session.find('/');
    const std::string head = session.substr(0, slash);
    auto it = forks_.find(head);
    if (it == forks_.end()) throw Error("no_session", "no session " + session);
    const bool destroy_this = slash == std::string::npos && msg.value("type", "") == "destroy";
    if (slash == std::string::npos) msg.erase("session");
    else msg["session"] = session.substr(slash + 1);
    if (destroy_this) {
        close_fork(head);
        return json{{"id", msg.value("id", json())}, {"ok", true}, {"result", json::object()}}.dump();
    }
    std::string line;
    if (!write_all(it->second.to_child, msg.dump() + "\n")) {
        close_fork(head);
        throw Error("no_session", "session " + session + " ended");
    }
    for (;;) {
        if (!it->second.from_child->next(line)) {
            close_fork(head);
            throw Error("no_session", "session " + session + " ended");
        }
        // Events the fork sends while it handles the request go out first,
        // tagged with the fork's session. Every line is written with its keys
        // sorted, so an event starts with {"event": and a reply ({"error"... or
        // {"id"...) never does.
        if (line.compare(0, 9, "{\"event\":") != 0) break;
        json ev;
        try { ev = json::parse(line); } catch (...) { throw Error("no_session", "session " + session + " sent a bad reply"); }
        ev["session"] = ev.contains("session") && ev["session"].is_string()
            ? head + "/" + ev["session"].get<std::string>() : head;
        send(ev);
    }
    if (msg.value("type", "") != "fork") return line;
    json reply;
    try { reply = json::parse(line); } catch (...) { throw Error("no_session", "session " + session + " sent a bad reply"); }
    if (!reply.value("ok", false) || !reply.contains("result")) return line;
    json& result = reply["result"];
    if (result.value("detached", false)) {
        const int fd = it->second.from_child->take_fd();
        if (fd < 0) throw Error("fork", "session " + session + " sent no socket for its detached fork");
        // Held here now: the fork that sent it may close its copy.
        write_all(it->second.to_child, "{\"type\":\"received\"}\n");
        if (is_fork_) { pass_fd_ = fd; return line; }
        const std::string id = std::to_string(next_fork_++);
        Fork f;
        f.to_child = fd;
        f.from_child = std::make_unique<LineReader>(fd, true);
        forks_.emplace(id, std::move(f));
        result["session"] = id;
        return reply.dump();
    }
    // A fork made inside the fork is addressed through it.
    if (result.contains("session")) result["session"] = head + "/" + result["session"].get<std::string>();
    return reply.dump();
}

// batch: the requests run in order, as if sent one by one, and one reply comes
// back for all of them. A request with "if" runs only when the request before
// it ran and replied with that `status` (or one of a list); a skipped one
// matches nothing. The first failing request ends the batch. Each result
// leaves out `state`: the batch's reply carries the state after it.
json Server::do_batch(const json& msg) {
    if (!msg.contains("requests") || !msg["requests"].is_array()) throw Error("bad_request", "batch needs a requests array");
    json results = json::array();
    bool prev_ran = false;
    std::string prev_status;
    for (const auto& req : msg["requests"]) {
        if (req.is_object() && req.contains("if")) {
            const json& cond = req["if"];
            auto matches = [&](const json& c) { return prev_ran && c.is_string() && c.get<std::string>() == prev_status; };
            bool run = false;
            if (cond.is_array()) { for (const auto& c : cond) run = run || matches(c); }
            else run = matches(cond);
            if (!run) {
                results.push_back({{"ok", true}, {"skipped", true}});
                prev_ran = false;
                prev_status.clear();
                continue;
            }
        }
        json error;
        try {
            if (!req.is_object()) throw Error("bad_request", "a batch request is a JSON object");
            const std::string type = req.value("type", "");
            if (type == "batch" || type == "fork" || type == "exit" || type == "answer")
                throw Error("bad_request", type + " cannot run in a batch");
            if (req.contains("session")) throw Error("bad_request", "a batch request has no session (the batch's applies)");
            json result = dispatch(type, req);
            prev_ran = true;
            prev_status = result.is_object() && result.contains("status") && result["status"].is_string()
                ? result["status"].get<std::string>() : std::string{};
            if (result.is_object()) result.erase("state");
            results.push_back({{"ok", true}, {"result", std::move(result)}});
            continue;
        } catch (const Error& e) {
            error = {{"code", e.code}, {"message", e.what()}};
        } catch (const nlohmann::json::exception& e) {
            error = {{"code", "bad_request"}, {"message", e.what()}};
        } catch (const std::exception& e) {
            error = {{"code", "internal"}, {"message", e.what()}};
        }
        results.push_back({{"ok", false}, {"error", std::move(error)}});
        break;
    }
    return {{"results", std::move(results)}, {"state", state()}};
}

json Server::dispatch(const std::string& type, const json& msg) {
    if (type == "hello") {
        return {{"protocol", protocol::VERSION}, {"version", OCGBRIDGE_VERSION}, {"cdb", CardDatabase::cdb_supported()},
                {"cards", cards_.size()}, {"fork", is_fork_}, {"pid", static_cast<long>(getpid())},
                {"queries", session_.query_names()}};
    }
    if (type == "loadCards") {
        if (msg.contains("scripts") && msg["scripts"].is_string()) scripts_ = msg["scripts"].get<std::string>();
        const long added = load_cards(msg);
        return {{"added", added}, {"cards", cards_.size()}};
    }
    if (type == "init") {
        protocol::InitOptions o = protocol::parse_init(msg);
        if (!o.scripts.empty()) scripts_ = o.scripts;
        session_.destroy();
        session_.set_scripts_path(scripts_);
        session_.responder() = o.responder;
        session_.set_chain_start_snapshots(o.chain_start_snapshots);
        session_.set_record_messages(o.record_messages);
        session_.set_script_dirs(o.script_dirs ? *o.script_dirs : script_dirs_);
        session_.set_preload_scripts(o.preload ? *o.preload : default_preload_scripts());
        install_event_filters(o);
        session_.set_ask_hook(nullptr);
        if (asks_) session_.set_ask_hook([this](Session& s, const json& p, std::vector<uint8_t>& r) { return ask(s, p, r); });
        try {
            session_.start(o.setup);
        } catch (const std::exception& ex) {
            throw Error("init", ex.what());
        }
        if (ext_.on_init) ext_.on_init(session_, msg);
        return {{"state", state()}};
    }
    if (type == "state") return {{"state", state()}};
    if (type == "batch") return do_batch(msg);
    if (type == "query") {
        if (!msg.contains("name") || !msg["name"].is_string()) throw Error("bad_request", "query needs a name");
        try {
            return {{"value", session_.query(msg["name"].get<std::string>(), msg.value("args", json::object()))}};
        } catch (const std::out_of_range& e) {
            throw Error("unknown_query", e.what());
        } catch (const std::invalid_argument& e) {
            throw Error("bad_request", e.what());
        }
    }
    if (!session_.active() && type != "destroy" && type != "exit" && type != "fork")
        throw Error("no_duel", "no duel: send init first");
    if (type == "respond") {
        if (!msg.contains("answer")) throw Error("bad_request", "respond needs an answer");
        std::vector<uint8_t> resp;
        std::string reason;
        if (!encode_answer(session_.pending_buf(), msg["answer"], resp, reason)) throw Error("illegal_answer", reason);
        session_.submit(resp);
        const std::string then = msg.value("then", std::string{"advance"});
        std::vector<Answered> log;
        AnswerStatus status = AnswerStatus::None;
        if (then == "advance") status = session_.advance(msg.value("max", 20), protocol::parse_hints(msg.value("hints", json())), false, &log);
        else if (then == "process") session_.process();
        else if (then != "none") throw Error("bad_request", "then must be \"advance\", \"process\" or \"none\"");
        return {{"status", protocol::status_name(status)}, {"answered", protocol::answered_json(log)}, {"state", state()}};
    }
    if (type == "process") {
        session_.process();
        return {{"state", state()}};
    }
    if (type == "advance" || type == "autoAnswer") {
        const Hints hints = protocol::parse_hints(msg.value("hints", json()));
        std::vector<Answered> log;
        AnswerStatus status = type == "autoAnswer"
            ? session_.auto_answer(hints, &log)
            : session_.advance(msg.value("max", 20), hints, msg.value("answerFirst", false), &log);
        return {{"status", protocol::status_name(status)}, {"answered", protocol::answered_json(log)}, {"state", state()}};
    }
    if (type == "encode") {
        std::vector<uint8_t> resp;
        std::string reason;
        if (!encode_answer(session_.pending_buf(), msg.value("answer", json()), resp, reason)) throw Error("illegal_answer", reason);
        return {{"raw", to_hex(resp)}};
    }
    if (type == "snapshot") return {{"players", session_.snapshot()}};
    if (type == "context") return session_.context(msg.value("player", -1));
    if (type == "take") {
        static const char* ALL[] = {"activations", "targets", "negations", "confirmed", "messages"};
        json which = msg.value("queues", json::array({"activations", "targets", "negations", "confirmed", "messages"}));
        json out = json::object();
        for (const auto& q : which) {
            const std::string name = q.is_string() ? q.get<std::string>() : "";
            if (name == ALL[0]) out[name] = session_.take_activations();
            else if (name == ALL[1]) out[name] = session_.take_targets();
            else if (name == ALL[2]) out[name] = session_.take_negations();
            else if (name == ALL[3]) out[name] = session_.take_confirmed();
            else if (name == ALL[4]) out[name] = session_.take_messages();
            else throw Error("bad_request", "unknown queue: " + q.dump());
        }
        return out;
    }
    if (type == "fork") return do_fork(msg);
    if (type == "destroy") {
        session_.destroy();
        return json::object();
    }
    if (type == "exit") return json::object();
    throw Error("unknown_request", "unknown request type: " + type);
}

// false: the process should exit.
bool Server::handle(const std::string& line) {
    for (int f : sent_fds_) close(f);
    sent_fds_.clear();
    json msg;
    try { msg = json::parse(line); } catch (const std::exception& ex) {
        send({{"id", nullptr}, {"ok", false}, {"error", {{"code", "bad_json"}, {"message", ex.what()}}}});
        return true;
    }
    if (!msg.is_object()) {
        send({{"id", nullptr}, {"ok", false}, {"error", {{"code", "bad_request"}, {"message", "a request is a JSON object"}}}});
        return true;
    }
    const json id = msg.value("id", json());
    const std::string type = msg.value("type", "");
    if (type == "answer") return true; // an answer to an ask that is no longer waited on
    if (type == "received") return true; // a passed socket arrived (sent_fds_ closed above)
    try {
        if (msg.contains("session") && !msg["session"].is_null()) {
            const std::string session = msg["session"].is_string() ? msg["session"].get<std::string>() : msg["session"].dump();
            reply_line(relay(session, msg));
            return true;
        }
        json result = dispatch(type, msg);
        if (just_forked_) { just_forked_ = false; return true; }
        reply_line(json{{"id", id}, {"ok", true}, {"result", result}}.dump());
        return type != "exit";
    } catch (const Error& e) {
        send({{"id", id}, {"ok", false}, {"error", {{"code", e.code}, {"message", e.what()}}}});
    } catch (const nlohmann::json::exception& e) {
        send({{"id", id}, {"ok", false}, {"error", {{"code", "bad_request"}, {"message", e.what()}}}});
    } catch (const std::exception& e) {
        send({{"id", id}, {"ok", false}, {"error", {{"code", "internal"}, {"message", e.what()}}}});
    }
    return true;
}

int Server::run(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_IGN);
    own_in_ = std::make_unique<LineReader>(STDIN_FILENO);
    in_ = own_in_.get();
    json load = json::object();
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&]() -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", a.c_str()); std::exit(2); }
            return argv[++i];
        };
        if (a == "--scripts") scripts_ = val();
        else if (a == "--cdb") { if (!load.contains("cdb")) load["cdb"] = json::array(); load["cdb"].push_back(val()); }
        else if (a == "--cards-json") load["json"] = val();
        else if (a == "--setcodes") load["setcodes"] = val();
        else if (a == "--archetypes") load["archetypes"] = val();
        else if (a == "--script-dirs") script_dirs_ = split_list(val());
        else if (a == "--version") { std::printf("ocgcore-bridge %s (protocol %d)\n", OCGBRIDGE_VERSION, protocol::VERSION); return 0; }
        else if (a == "--help" || a == "-h") {
            std::printf("usage: ocgcore-bridge [--scripts DIR] [--script-dirs SUB,SUB...] [--cdb FILE]...\n"
                        "                      [--cards-json FILE [--setcodes FILE] [--archetypes FILE]]\n"
                        "Newline-delimited JSON on stdin/stdout, protocol v%d (docs/protocol.md).\n", protocol::VERSION);
            return 0;
        } else { std::fprintf(stderr, "unknown argument: %s\n", a.c_str()); return 2; }
    }
    // Ready once the command line's card data is loaded: a client's request
    // timeouts never include the load.
    try {
        load_cards(load);
    } catch (const Error& e) {
        std::fprintf(stderr, "[ocgcore-bridge] %s\n", e.what());
    }
    session_.set_script_dirs(script_dirs_);
    if (ext_.setup) ext_.setup(session_);
    send({{"type", "ready"}, {"protocol", protocol::VERSION}, {"version", OCGBRIDGE_VERSION}});
    std::string line;
    while (next_line(line)) {
        if (line.empty()) continue;
        if (!handle(line)) break;
    }
    if (is_fork_) _exit(0); // a fork: no destructors, nothing of the parent's flushed
    for (auto it = forks_.begin(); it != forks_.end();) { auto id = it->first; ++it; close_fork(id); }
    reap(true);
    session_.destroy();
    return 0;
}

} // namespace

namespace ocgbridge::server {

int run(int argc, char** argv, const Extensions& ext) {
    setvbuf(stderr, nullptr, _IOLBF, 0);
    Server server(ext);
    return server.run(argc, argv);
}

} // namespace ocgbridge::server
