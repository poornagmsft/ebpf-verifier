// Copyright (c) Prevail Verifier contributors.
// SPDX-License-Identifier: Apache-2.0
#pragma once

/*
 * Build a CFG to interface with the abstract domains and fixpoint
 * iterators.
 *
 * All the CFG statements are strongly typed. However, only variables
 * need to be typed. The types of constants can be inferred from the
 * context since they always appear together with at least one
 * variable.
 *
 */
#include <memory>
#include <map>
#include <set>
#include <variant>
#include <vector>

#include <boost/iterator/transform_iterator.hpp>
#include <boost/range/iterator_range.hpp>
#include <boost/lexical_cast.hpp>

#include "crab/variable.hpp"
#include "crab_utils/bignums.hpp"
#include "crab_utils/debug.hpp"

#include "asm_syntax.hpp"
#include "asm_ostream.hpp"

namespace crab {

class cfg_t;

class basic_block_t final {
    friend class cfg_t;

  private:

  public:
    basic_block_t(const basic_block_t&) = delete;

    using label_vec_t = std::set<label_t>;
    using stmt_list_t = std::vector<Instruction>;
    using succ_iterator = label_vec_t::iterator;
    using const_succ_iterator = label_vec_t::const_iterator;
    using pred_iterator = succ_iterator;
    using const_pred_iterator = const_succ_iterator;
    using iterator = typename stmt_list_t::iterator;
    using const_iterator = typename stmt_list_t::const_iterator;
    using reverse_iterator = typename stmt_list_t::reverse_iterator;
    using const_reverse_iterator = typename stmt_list_t::const_reverse_iterator;

  private:
    label_t m_label;
    stmt_list_t m_ts;
    label_vec_t m_prev, m_next;

  public:
    template <typename T, typename... Args>
    void insert(Args&&... args) {
        m_ts.emplace_back(T{std::forward<Args>(args)...});
    }

    void insert(const Instruction& arg) { m_ts.push_back(arg); }

    explicit basic_block_t(label_t _label) : m_label(_label) {}

    ~basic_block_t() = default;

    [[nodiscard]] label_t label() const { return m_label; }

    iterator begin() { return (m_ts.begin()); }
    iterator end() { return (m_ts.end()); }
    [[nodiscard]] const_iterator begin() const { return (m_ts.begin()); }
    [[nodiscard]] const_iterator end() const { return (m_ts.end()); }

    reverse_iterator rbegin() { return (m_ts.rbegin()); }
    reverse_iterator rend() { return (m_ts.rend()); }
    [[nodiscard]] const_reverse_iterator rbegin() const { return (m_ts.rbegin()); }
    [[nodiscard]] const_reverse_iterator rend() const { return (m_ts.rend()); }

    [[nodiscard]] size_t size() const { return static_cast<size_t>(std::distance(begin(), end())); }

    std::pair<succ_iterator, succ_iterator> next_blocks() { return std::make_pair(m_next.begin(), m_next.end()); }

    std::pair<pred_iterator, pred_iterator> prev_blocks() { return std::make_pair(m_prev.begin(), m_prev.end()); }

    [[nodiscard]] std::pair<const_succ_iterator, const_succ_iterator> next_blocks() const {
        return std::make_pair(m_next.begin(), m_next.end());
    }

    [[nodiscard]] std::pair<const_pred_iterator, const_pred_iterator> prev_blocks() const {
        return std::make_pair(m_prev.begin(), m_prev.end());
    }

    [[nodiscard]] const label_vec_t& next_blocks_set() const {
        return m_next;
    }

    [[nodiscard]] const label_vec_t& prev_blocks_set() const {
        return m_prev;
    }

    // Add a cfg_t edge from *this to b
    void operator>>(basic_block_t& b) {
        m_next.insert(b.m_label);
        b.m_prev.insert(m_label);
    }

    // Remove a cfg_t edge from *this to b
    void operator-=(basic_block_t& b) {
        m_next.erase(b.m_label);
        b.m_prev.erase(m_label);
    }

    // insert all statements of other at the back
    void move_back(basic_block_t& other) {
        m_ts.reserve(m_ts.size() + other.m_ts.size());
        std::move(other.m_ts.begin(), other.m_ts.end(), std::back_inserter(m_ts));
    }

    [[nodiscard]] size_t in_degree() const {
        return m_prev.size();
    }

    [[nodiscard]] size_t out_degree() const {
        return m_next.size();
    }

    void swap_instructions(stmt_list_t& ts) { std::swap(m_ts, ts); }
};

// Viewing basic_block_t with all statements reversed. Useful for
// backward analysis.
class basic_block_rev_t final {
  public:
    using succ_iterator = typename basic_block_t::succ_iterator;
    using const_succ_iterator = typename basic_block_t::const_succ_iterator;
    using pred_iterator = succ_iterator;
    using const_pred_iterator = const_succ_iterator;

    using iterator = typename basic_block_t::reverse_iterator;
    using const_iterator = typename basic_block_t::const_reverse_iterator;

  public:
    basic_block_t& _bb;

    explicit basic_block_rev_t(basic_block_t& bb) : _bb(bb) {}

    [[nodiscard]] label_t label() const { return _bb.label(); }

    iterator begin() { return _bb.rbegin(); }

    iterator end() { return _bb.rend(); }

    [[nodiscard]] const_iterator begin() const { return _bb.rbegin(); }

    [[nodiscard]] const_iterator end() const { return _bb.rend(); }

    [[nodiscard]] std::size_t size() const { return static_cast<size_t>(std::distance(begin(), end())); }

    std::pair<succ_iterator, succ_iterator> next_blocks() { return _bb.prev_blocks(); }

    std::pair<pred_iterator, pred_iterator> prev_blocks() { return _bb.next_blocks(); }

    [[nodiscard]] std::pair<const_succ_iterator, const_succ_iterator> next_blocks() const { return _bb.prev_blocks(); }

    [[nodiscard]] std::pair<const_pred_iterator, const_pred_iterator> prev_blocks() const { return _bb.next_blocks(); }


    [[nodiscard]] const basic_block_t::label_vec_t& next_blocks_set() const {
        return _bb.prev_blocks_set();
    }

    [[nodiscard]] const basic_block_t::label_vec_t& prev_blocks_set() const {
        return _bb.next_blocks_set();
    }
};

/// Control-Flow Graph.
class cfg_t final {
  public:
    using node_t = label_t; // for Bgl graphs

    using succ_iterator = typename basic_block_t::succ_iterator;
    using pred_iterator = typename basic_block_t::pred_iterator;
    using const_succ_iterator = typename basic_block_t::const_succ_iterator;
    using const_pred_iterator = typename basic_block_t::const_pred_iterator;

    using succ_range = boost::iterator_range<succ_iterator>;
    using pred_range = boost::iterator_range<pred_iterator>;
    using const_succ_range = boost::iterator_range<const_succ_iterator>;
    using const_pred_range = boost::iterator_range<const_pred_iterator>;

  private:
    using basic_block_map_t = std::map<label_t, basic_block_t>;
    using binding_t = typename basic_block_map_t::value_type;

    struct get_label {
        label_t operator()(const binding_t& p) const { return p.second.label(); }
    };

  public:
    using iterator = typename basic_block_map_t::iterator;
    using const_iterator = typename basic_block_map_t::const_iterator;
    using label_iterator = boost::transform_iterator<get_label, typename basic_block_map_t::iterator>;
    using const_label_iterator = boost::transform_iterator<get_label, typename basic_block_map_t::const_iterator>;

  private:
    label_t m_entry;
    label_t m_exit;
    basic_block_map_t m_blocks;

    using visited_t = std::set<label_t>;
    template <typename T>
    void dfs_rec(const label_t& curId, visited_t& visited, T f) const {
        if (!visited.insert(curId).second)
            return;

        const auto& cur = get_node(curId);
        f(cur);
        for (const auto& n : boost::make_iterator_range(cur.next_blocks())) {
            dfs_rec(n, visited, f);
        }
    }
  public:

    template <typename T>
    void dfs(T f) const {
        visited_t visited;
        dfs_rec(m_entry, visited, f);
    }

    cfg_t(const label_t& entry, const label_t& exit) : m_entry(entry), m_exit(exit) {
        m_blocks.emplace(entry, entry);
        m_blocks.emplace(exit, exit);
    }

    cfg_t(const cfg_t&) = delete;

    cfg_t(cfg_t&& o) noexcept
        : m_entry(std::move(o.m_entry)), m_exit(std::move(o.m_exit)), m_blocks(std::move(o.m_blocks)) {}

    ~cfg_t() = default;

    label_t exit() const { return m_exit; }

    // --- Begin ikos fixpoint API

    label_t entry() const { return m_entry; }

    const_succ_range next_nodes(const label_t& _label) const {
        return boost::make_iterator_range(get_node(_label).next_blocks());
    }

    const_pred_range prev_nodes(const label_t& _label) const {
        return boost::make_iterator_range(get_node(_label).prev_blocks());
    }

    succ_range next_nodes(const label_t& _label) { return boost::make_iterator_range(get_node(_label).next_blocks()); }

    pred_range prev_nodes(const label_t& _label) { return boost::make_iterator_range(get_node(_label).prev_blocks()); }

    basic_block_t& get_node(const label_t& _label) {
        auto it = m_blocks.find(_label);
        if (it == m_blocks.end()) {
            CRAB_ERROR("Basic block ", _label, " not found in the CFG: ", __LINE__);
        }
        return it->second;
    }

    const basic_block_t& get_node(const label_t& _label) const {
        auto it = m_blocks.find(_label);
        if (it == m_blocks.end()) {
            CRAB_ERROR("Basic block ", _label, " not found in the CFG: ", __LINE__);
        }
        return it->second;
    }

    // --- End ikos fixpoint API

    basic_block_t& insert(const label_t& _label) {
        auto it = m_blocks.find(_label);
        if (it != m_blocks.end())
            return it->second;

        m_blocks.emplace(_label, _label);
        return get_node(_label);
    }

    void remove(const label_t& _label) {
        if (_label == m_entry)
            CRAB_ERROR("Cannot remove entry block");

        if (_label == m_exit)
            CRAB_ERROR("Cannot remove exit block");

        std::vector<std::pair<basic_block_t*, basic_block_t*>> dead_edges;
        auto& bb = get_node(_label);

        for (const auto& id : boost::make_iterator_range(bb.prev_blocks())) {
            if (_label != id) {
                dead_edges.emplace_back(&get_node(id), &bb);
            }
        }

        for (const auto& id : boost::make_iterator_range(bb.next_blocks())) {
            if (_label != id) {
                dead_edges.emplace_back(&bb, &get_node(id));
            }
        }

        for (auto p : dead_edges) {
            (*p.first) -= (*p.second);
        }

        m_blocks.erase(_label);
    }

    //! return a begin iterator of basic_block_t's
    iterator begin() { return m_blocks.begin(); }

    //! return an end iterator of basic_block_t's
    iterator end() { return m_blocks.end(); }

    const_iterator begin() const { return m_blocks.begin(); }

    const_iterator end() const { return m_blocks.end(); }

    //! return a begin iterator of label_t's
    label_iterator label_begin() { return boost::make_transform_iterator(m_blocks.begin(), get_label()); }

    //! return an end iterator of label_t's
    label_iterator label_end() { return boost::make_transform_iterator(m_blocks.end(), get_label()); }

    //! return a begin iterator of label_t's
    std::vector<label_t> labels() const {
        std::vector<label_t> res;
        res.reserve(m_blocks.size());
        for (const auto& p : m_blocks)
            res.push_back(p.first);
        return res;
    }

    size_t size() const { return static_cast<size_t>(std::distance(begin(), end())); }

    void simplify() {
        std::set<label_t> worklist(this->label_begin(), this->label_end());
        while (!worklist.empty()) {
            label_t label = *worklist.begin();
            worklist.erase(label);

            basic_block_t& bb = get_node(label);
            if (bb.in_degree() == 1 && get_parent(label).out_degree() == 1) {
                continue;
            }
            while (bb.out_degree() == 1) {
                basic_block_t& next_bb = get_child(label);

                if (&next_bb == &bb || next_bb.in_degree() != 1) {
                    break;
                }

                worklist.erase(next_bb.label());

                if (next_bb.label() == m_exit) {
                    m_exit = label;
                }

                bb.move_back(next_bb);
                bb -= next_bb;
                auto children = next_bb.m_next;
                for (const label_t& next_next_label : children) {
                    basic_block_t& next_next_bb = get_node(next_next_label);
                    bb >> next_next_bb;
                }

                // delete next_bb entirely
                remove(next_bb.label());
            }
        }
    }

    [[nodiscard]] std::vector<label_t> sorted_labels() const {
        std::vector<label_t> labels = this->labels();
        std::sort(labels.begin(), labels.end());
        return labels;
    }

  private:
    // Helpers
    bool has_one_child(const label_t& b) const {
        auto rng = next_nodes(b);
        return (std::distance(rng.begin(), rng.end()) == 1);
    }

    bool has_one_parent(const label_t& b) const {
        auto rng = prev_nodes(b);
        return (std::distance(rng.begin(), rng.end()) == 1);
    }

    basic_block_t& get_child(const label_t& b) {
        assert(has_one_child(b));
        auto rng = next_nodes(b);
        return get_node(*(rng.begin()));
    }

    basic_block_t& get_parent(const label_t& b) {
        assert(has_one_parent(b));
        auto rng = prev_nodes(b);
        return get_node(*(rng.begin()));
    }

    // mark reachable blocks from curId
    template <class AnyCfg>
    void mark_alive_blocks(label_t curId, AnyCfg& cfg_t, visited_t& visited) {
        if (visited.count(curId) > 0)
            return;
        visited.insert(curId);
        for (const auto& child : cfg_t.next_nodes(curId)) {
            mark_alive_blocks(child, cfg_t, visited);
        }
    }

    void remove_unreachable_blocks();

    // remove blocks that cannot reach the exit block
    void remove_useless_blocks();
};

// Viewing a cfg_t with all edges and block statements reversed. Useful for backward analysis.
class cfg_rev_t final {
  public:
    using node_t = label_t; // for Bgl graphs

    using pred_range = typename cfg_t::succ_range;
    using succ_range = typename cfg_t::pred_range;
    using const_pred_range = typename cfg_t::const_succ_range;
    using const_succ_range = typename cfg_t::const_pred_range;

    // For BGL
    using succ_iterator = typename basic_block_t::succ_iterator;
    using pred_iterator = typename basic_block_t::pred_iterator;
    using const_succ_iterator = typename basic_block_t::const_succ_iterator;
    using const_pred_iterator = typename basic_block_t::const_pred_iterator;

  private:
    using visited_t = std::set<label_t>;

    template <typename T>
    void dfs_rec(const label_t& curId, visited_t& visited, T f) const {
        if (!visited.insert(curId).second)
            return;
        f(get_node(curId));
        for (const auto& n : next_nodes(curId)) {
            dfs_rec(n, visited, f);
        }
    }

    template <typename T>
    void dfs(T f) const {
        visited_t visited;
        dfs_rec(entry(), visited, f);
    }

  public:
    using basic_block_rev_map_t = std::map<label_t, basic_block_rev_t>;
    using iterator = typename basic_block_rev_map_t::iterator;
    using const_iterator = typename basic_block_rev_map_t::const_iterator;
    using label_iterator = typename cfg_t::label_iterator;
    using const_label_iterator = typename cfg_t::const_label_iterator;

  private:
    cfg_t& _cfg;
    basic_block_rev_map_t _rev_bbs;

  public:
    explicit cfg_rev_t(cfg_t& cfg) : _cfg(cfg) {
        // Create basic_block_rev_t from basic_block_t objects
        // Note that basic_block_rev_t is also a view of basic_block_t so it
        // doesn't modify basic_block_t objects.
        for (auto& [label, bb] : cfg) {
            _rev_bbs.emplace(label, bb);
        }
    }

    cfg_rev_t(const cfg_rev_t& o) = default;

    cfg_rev_t(cfg_rev_t&& o) noexcept : _cfg(o._cfg), _rev_bbs(std::move(o._rev_bbs)) {}

    label_t entry() const { return _cfg.exit(); }

    const_succ_range next_nodes(const label_t& bb) const { return _cfg.prev_nodes(bb); }

    const_pred_range prev_nodes(const label_t& bb) const { return _cfg.next_nodes(bb); }

    succ_range next_nodes(const label_t& bb) { return _cfg.prev_nodes(bb); }

    pred_range prev_nodes(const label_t& bb) { return _cfg.next_nodes(bb); }

    basic_block_rev_t& get_node(const label_t& _label) {
        auto it = _rev_bbs.find(_label);
        if (it == _rev_bbs.end())
            CRAB_ERROR("Basic block ", _label, " not found in the CFG: ", __LINE__);
        return it->second;
    }

    const basic_block_rev_t& get_node(const label_t& _label) const {
        auto it = _rev_bbs.find(_label);
        if (it == _rev_bbs.end())
            CRAB_ERROR("Basic block ", _label, " not found in the CFG: ", __LINE__);
        return it->second;
    }

    iterator begin() { return _rev_bbs.begin(); }

    iterator end() { return _rev_bbs.end(); }

    const_iterator begin() const { return _rev_bbs.begin(); }

    const_iterator end() const { return _rev_bbs.end(); }

    label_iterator label_begin() { return _cfg.label_begin(); }

    label_iterator label_end() { return _cfg.label_end(); }

    label_t exit() const { return _cfg.entry(); }
};

inline void cfg_t::remove_useless_blocks() {
    cfg_rev_t rev_cfg(*this);

    visited_t useful, useless;
    mark_alive_blocks(rev_cfg.entry(), rev_cfg, useful);

    if (!useful.count(m_exit))
        CRAB_ERROR("Exit block must be reachable");
    for (auto const& label : labels()) {
        if (!useful.count(label)) {
            useless.insert(label);
        }
    }

    for (const auto& _label : useless) {
        remove(_label);
    }
}

inline void cfg_t::remove_unreachable_blocks() {
    visited_t alive, dead;
    mark_alive_blocks(entry(), *this, alive);

    for (auto const& label : labels()) {
        if (alive.count(label) <= 0) {
            dead.insert(label);
        }
    }

    if (dead.count(m_exit))
        CRAB_ERROR("Exit block must be reachable");
    for (const auto& _label : dead) {
        remove(_label);
    }
}

} // end namespace crab

using crab::basic_block_t;
using crab::cfg_t;

std::vector<std::string> stats_headers();

std::map<std::string, int> collect_stats(const cfg_t&);

cfg_t prepare_cfg(const InstructionSeq& prog, const program_info& info, bool simplify);

void explicate_assertions(cfg_t& cfg, const program_info& info);

void print_dot(const cfg_t& cfg, std::ostream& out);
void print_dot(const cfg_t& cfg, const std::string& outfile);

std::ostream& operator<<(std::ostream& o, const crab::basic_block_t& bb);
std::ostream& operator<<(std::ostream& o, const crab::basic_block_rev_t& bb);
std::ostream& operator<<(std::ostream& o, const cfg_t& cfg);
