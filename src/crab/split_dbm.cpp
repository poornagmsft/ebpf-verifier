// Copyright (c) Prevail Verifier contributors.
// SPDX-License-Identifier: Apache-2.0
#include "crab/split_dbm.hpp"

#include <utility>

#include "crab_utils/debug.hpp"
#include "crab_utils/stats.hpp"

namespace crab::domains {

SplitDBM::vert_id SplitDBM::get_vert(variable_t v) {
    auto it = vert_map.find(v);
    if (it != vert_map.end())
        return (*it).second;

    vert_id vert(g.new_vertex());
    vert_map.insert(vmap_elt_t(v, vert));
    // Initialize
    assert(vert <= rev_map.size());
    if (vert < rev_map.size()) {
        potential[vert] = Wt(0);
        rev_map[vert] = v;
    } else {
        potential.emplace_back(0);
        rev_map.push_back(v);
    }
    vert_map.insert(vmap_elt_t(v, vert));

    assert(vert != 0);

    return vert;
}

void SplitDBM::close_over_edge(vert_id ii, vert_id jj) {
    assert(ii != 0 && jj != 0);
    SubGraph<graph_t> g_excl(g, 0);

    Wt c = g_excl.edge_val(ii, jj);

    typename graph_t::mut_val_ref_t w;

    // There may be a cheaper way to do this.
    // GKG: Now implemented.
    std::vector<std::pair<vert_id, Wt>> src_dec;
    for (auto edge : g_excl.e_preds(ii)) {
        vert_id se = edge.vert;
        Wt wt_sij = edge.val + c;

        assert(g_excl.succs(se).begin() != g_excl.succs(se).end());
        if (se != jj) {
            if (g_excl.lookup(se, jj, &w)) {
                if (w.get() <= wt_sij)
                    continue;

                w = wt_sij;
            } else {
                g_excl.add_edge(se, wt_sij, jj);
            }
            src_dec.emplace_back(se, edge.val);
        }
    }

    std::vector<std::pair<vert_id, Wt>> dest_dec;
    for (auto edge : g_excl.e_succs(jj)) {
        vert_id de = edge.vert;
        Wt wt_ijd = edge.val + c;
        if (de != ii) {
            if (g_excl.lookup(ii, de, &w)) {
                if (w.get() <= wt_ijd)
                    continue;
                w = wt_ijd;
            } else {
                g_excl.add_edge(ii, wt_ijd, de);
            }
            dest_dec.emplace_back(de, edge.val);
        }
    }

    for (auto [se, p1] : src_dec) {
        Wt wt_sij = c + p1;
        for (auto [de, p2] : dest_dec) {
            Wt wt_sijd = wt_sij + p2;
            if (g.lookup(se, de, &w)) {
                if (w.get() <= wt_sijd)
                    continue;
                w = wt_sijd;
            } else {
                g.add_edge(se, wt_sijd, de);
            }
        }
    }

    // Closure is now updated.
}

void SplitDBM::diffcsts_of_assign(variable_t x, const linear_expression_t& exp,
                                  /* if true then process the upper
                                     bounds, else the lower bounds */
                                  bool extract_upper_bounds,
                                  /* foreach {v, k} \in diff_csts we have
                                     the difference constraint v - k <= k */
                                  std::vector<std::pair<variable_t, Wt>>& diff_csts) {

    std::optional<variable_t> unbounded_var;
    std::vector<std::pair<variable_t, Wt>> terms;
    bool overflow;

    Wt residual(convert_NtoW(exp.constant(), overflow));
    if (overflow) {
        return;
    }

    for (auto [y, n] : exp) {
        Wt coeff(convert_NtoW(n, overflow));
        if (overflow) {
            continue;
        }

        if (coeff < Wt(0)) {
            // Can't do anything with negative coefficients.
            auto y_val = (extract_upper_bounds ? operator[](y).lb() : operator[](y).ub());

            if (y_val.is_infinite()) {
                return;
            }
            residual += convert_NtoW(*(y_val.number()), overflow) * coeff;
            if (overflow) {
                continue;
            }

        } else {
            auto y_val = (extract_upper_bounds ? operator[](y).ub() : operator[](y).lb());

            if (y_val.is_infinite()) {
                if (unbounded_var || coeff != Wt(1)) {
                    return;
                }
                unbounded_var = y;
            } else {
                Wt ymax(convert_NtoW(*(y_val.number()), overflow));
                if (overflow) {
                    continue;
                }
                residual += ymax * coeff;
                terms.emplace_back(y, ymax);
            }
        }
    }

    if (unbounded_var) {
        // There is exactly one unbounded variable with unit
        // coefficient
        diff_csts.emplace_back(*unbounded_var, residual);
    } else {
        for (auto [v, n] : terms) {
            diff_csts.emplace_back(v, residual - n);
        }
    }
}

void SplitDBM::diffcsts_of_lin_leq(const linear_expression_t& exp,
                                   /* difference contraints */
                                   std::vector<diffcst_t>& csts,
                                   /* x >= lb for each {x,lb} in lbs */
                                   std::vector<std::pair<variable_t, Wt>>& lbs,
                                   /* x <= ub for each {x,ub} in ubs */
                                   std::vector<std::pair<variable_t, Wt>>& ubs) {
    bool underflow, overflow;

    Wt exp_ub = -(convert_NtoW(exp.constant(), overflow));
    if (overflow) {
        return;
    }

    // temporary hack
    convert_NtoW(exp.constant() - 1, underflow);
    if (underflow) {
        // We don't like MIN either because the code will compute
        // minus MIN and it will silently overflow.
        return;
    }

    Wt unbounded_lbcoeff;
    Wt unbounded_ubcoeff;
    std::optional<variable_t> unbounded_lbvar;
    std::optional<variable_t> unbounded_ubvar;

    std::vector<std::pair<std::pair<Wt, variable_t>, Wt>> pos_terms, neg_terms;
    for (auto [y, n] : exp) {
        Wt coeff(convert_NtoW(n, overflow));
        if (overflow) {
            continue;
        }
        if (coeff > Wt(0)) {
            auto y_lb = operator[](y).lb();
            if (y_lb.is_infinite()) {
                if (unbounded_lbvar) {
                    return;
                }
                unbounded_lbvar = y;
                unbounded_lbcoeff = coeff;
            } else {
                Wt ymin(convert_NtoW(*(y_lb.number()), overflow));
                if (overflow) {
                    continue;
                }
                exp_ub -= ymin * coeff;
                pos_terms.push_back({{coeff, y}, ymin});
            }
        } else {
            auto y_ub = operator[](y).ub();
            if (y_ub.is_infinite()) {
                if (unbounded_ubvar) {
                    return;
                }
                unbounded_ubvar = y;
                unbounded_ubcoeff = -coeff;
            } else {
                Wt ymax(convert_NtoW(*(y_ub.number()), overflow));
                if (overflow) {
                    continue;
                }
                exp_ub -= ymax * coeff;
                neg_terms.push_back({{-coeff, y}, ymax});
            }
        }
    }

    if (unbounded_lbvar) {
        variable_t x(*unbounded_lbvar);
        if (unbounded_ubvar) {
            if (unbounded_lbcoeff != Wt(1) || unbounded_ubcoeff != Wt(1)) {
                return;
            }
            variable_t y(*unbounded_ubvar);
            csts.push_back({{x, y}, exp_ub});
        } else {
            if (unbounded_lbcoeff == Wt(1)) {
                for (auto [nv, k] : neg_terms) {
                    csts.push_back({{x, nv.second}, exp_ub - k});
                }
            }
            // Add bounds for x
            ubs.emplace_back(x, exp_ub / unbounded_lbcoeff);
        }
    } else {
        if (unbounded_ubvar) {
            variable_t y(*unbounded_ubvar);
            if (unbounded_ubcoeff == Wt(1)) {
                for (auto [nv, k] : pos_terms) {
                    csts.push_back({{nv.second, y}, exp_ub + k});
                }
            }
            // Add bounds for y
            lbs.emplace_back(y, -exp_ub / unbounded_ubcoeff);
        } else {
            for (auto [neg_nv, neg_k] : neg_terms) {
                for (auto [pos_nv, pos_k] : pos_terms) {
                    csts.push_back({{pos_nv.second, neg_nv.second}, exp_ub - neg_k + pos_k});
                }
            }
            for (auto [neg_nv, neg_k] : neg_terms) {
                lbs.emplace_back(neg_nv.second, -exp_ub / neg_nv.first + neg_k);
            }
            for (auto [pos_nv, pos_k] : pos_terms) {
                ubs.emplace_back(pos_nv.second, exp_ub / pos_nv.first + pos_k);
            }
        }
    }
}

bool SplitDBM::add_linear_leq(const linear_expression_t& exp) {
    std::vector<std::pair<variable_t, Wt>> lbs, ubs;
    std::vector<diffcst_t> csts;
    diffcsts_of_lin_leq(exp, csts, lbs, ubs);

    typename graph_t::mut_val_ref_t w;
    for (auto [var, n] : lbs) {
        CRAB_LOG("zones-split", std::cout << var << ">=" << n << "\n");
        vert_id vert = get_vert(var);
        if (g.lookup(vert, 0, &w) && w.get() <= -n)
            continue;
        g.set_edge(vert, -n, 0);

        if (!repair_potential(vert, 0)) {
            set_to_bottom();
            return false;
        }
    }
    for (auto [var, n] : ubs) {
        CRAB_LOG("zones-split", std::cout << var << "<=" << n << "\n");
        vert_id vert = get_vert(var);
        if (g.lookup(0, vert, &w) && w.get() <= n)
            continue;
        g.set_edge(0, n, vert);
        if (!repair_potential(0, vert)) {
            set_to_bottom();
            return false;
        }
    }

    for (auto [diff, k] : csts) {
        CRAB_LOG("zones-split", std::cout << diff.first << "-" << diff.second << "<=" << k << "\n");

        vert_id src = get_vert(diff.second);
        vert_id dest = get_vert(diff.first);
        g.update_edge(src, k, dest);
        if (!repair_potential(src, dest)) {
            set_to_bottom();
            return false;
        }
        close_over_edge(src, dest);
    }
    // Collect bounds
    // GKG: Now done in close_over_edge

    edge_vector delta;
    GrOps::close_after_assign(g, potential, 0, delta);
    GrOps::apply_delta(g, delta);
    // CRAB_WARN("SplitDBM::add_linear_leq not yet implemented.");
    return true;
}

void SplitDBM::add_univar_disequation(variable_t x, const number_t& n) {
    bool overflow;
    interval_t i = get_interval(x);
    interval_t new_i = trim_interval(i, interval_t(n));
    if (new_i.is_bottom()) {
        set_to_bottom();
    } else if (!new_i.is_top() && (new_i <= i)) {
        vert_id v = get_vert(x);
        typename graph_t::mut_val_ref_t w;
        if (new_i.lb().is_finite()) {
            // strenghten lb
            Wt lb_val = convert_NtoW(-(*(new_i.lb().number())), overflow);
            if (overflow) {
                return;
            }

            if (g.lookup(v, 0, &w) && lb_val < w.get()) {
                g.set_edge(v, lb_val, 0);
                if (!repair_potential(v, 0)) {
                    set_to_bottom();
                    return;
                }
                // Update other bounds
                for (auto e : g.e_preds(v)) {
                    if (e.vert == 0)
                        continue;
                    g.update_edge(e.vert, e.val + lb_val, 0);
                    if (!repair_potential(e.vert, 0)) {
                        set_to_bottom();
                        return;
                    }
                }
            }
        }
        if (new_i.ub().is_finite()) {
            // strengthen ub
            Wt ub_val = convert_NtoW(*(new_i.ub().number()), overflow);
            if (overflow) {
                return;
            }

            if (g.lookup(0, v, &w) && (ub_val < w.get())) {
                g.set_edge(0, ub_val, v);
                if (!repair_potential(0, v)) {
                    set_to_bottom();
                    return;
                }
                // Update other bounds
                for (auto e : g.e_succs(v)) {
                    if (e.vert == 0)
                        continue;
                    g.update_edge(0, e.val + ub_val, e.vert);
                    if (!repair_potential(0, e.vert)) {
                        set_to_bottom();
                        return;
                    }
                }
            }
        }
    }
}

bool SplitDBM::operator<=(SplitDBM o) {
    CrabStats::count("SplitDBM.count.leq");
    ScopedCrabStats __st__("SplitDBM.leq");

    // cover all trivial cases to avoid allocating a dbm matrix
    if (is_bottom())
        return true;
    else if (o.is_bottom())
        return false;
    else if (o.is_top())
        return true;
    else if (is_top())
        return false;
    else {
        normalize();

        // CRAB_LOG("zones-split", std::cout << "operator<=: "<< *this<< "<=?"<< o <<"\n");

        if (vert_map.size() < o.vert_map.size())
            return false;

        typename graph_t::mut_val_ref_t wx;
        typename graph_t::mut_val_ref_t wy;

        // Set up a mapping from o to this.
        std::vector<unsigned int> vert_renaming(o.g.size(), -1);
        vert_renaming[0] = 0;
        for (auto [v, n] : o.vert_map) {
            if (o.g.succs(n).size() == 0 && o.g.preds(n).size() == 0)
                continue;

            auto it = vert_map.find(v);
            // We can't have this <= o if we're missing some
            // vertex.
            if (it == vert_map.end())
                return false;
            vert_renaming[n] = it->second;
            // vert_renaming[(*it).second] = p.second;
        }

        assert(g.size() > 0);
        // GrPerm g_perm(vert_renaming, g);

        for (vert_id ox : o.g.verts()) {
            if (o.g.succs(ox).size() == 0)
                continue;

            assert(vert_renaming[ox] != (unsigned)-1);
            vert_id x = vert_renaming[ox];
            for (auto edge : o.g.e_succs(ox)) {
                vert_id oy = edge.vert;
                assert(vert_renaming[oy] != (unsigned)-1);
                vert_id y = vert_renaming[oy];
                Wt ow = edge.val;

                if (g.lookup(x, y, &wx) && (wx.get() <= ow))
                    continue;

                if (!g.lookup(x, 0, &wx) || !g.lookup(0, y, &wy))
                    return false;
                if (!(wx.get() + wy.get() <= ow))
                    return false;
            }
        }
        return true;
    }
}

SplitDBM SplitDBM::operator|(const SplitDBM& _o) & {
    CrabStats::count("SplitDBM.count.join");
    ScopedCrabStats __st__("SplitDBM.join");

    if (is_bottom() || _o.is_top())
        return _o;
    else if (is_top() || _o.is_bottom())
        return *this;
    SplitDBM o{_o};
    CRAB_LOG("zones-split", std::cout << "Before join:\n"
                                      << "DBM 1\n"
                                      << *this << "\n"
                                      << "DBM 2\n"
                                      << o << "\n");

    normalize();
    o.normalize();

    // Figure out the common renaming, initializing the
    // resulting potentials as we go.
    std::vector<vert_id> perm_x;
    std::vector<vert_id> perm_y;
    std::vector<variable_t> perm_inv;

    std::vector<Wt> pot_rx;
    std::vector<Wt> pot_ry;
    vert_map_t out_vmap;
    rev_map_t out_revmap;
    // Add the zero vertex
    assert(!potential.empty());
    pot_rx.emplace_back(0);
    pot_ry.emplace_back(0);
    perm_x.push_back(0);
    perm_y.push_back(0);
    out_revmap.push_back(std::nullopt);

    for (auto [v, n] : vert_map) {
        auto it = o.vert_map.find(v);
        // Variable exists in both
        if (it != o.vert_map.end()) {
            out_vmap.insert(vmap_elt_t(v, static_cast<vert_id>(perm_x.size())));
            out_revmap.push_back(v);

            pot_rx.push_back(potential[n] - potential[0]);
            // XXX JNL: check this out
            // pot_ry.push_back(o.potential[p.second] - o.potential[0]);
            pot_ry.push_back(o.potential[it->second] - o.potential[0]);
            perm_inv.push_back(v);
            perm_x.push_back(n);
            perm_y.push_back(it->second);
        }
    }
    size_t sz = perm_x.size();

    // Build the permuted view of x and y.
    assert(g.size() > 0);
    GrPerm gx(perm_x, g);
    assert(o.g.size() > 0);
    GrPerm gy(perm_y, o.g);

    // Compute the deferred relations
    graph_t g_ix_ry;
    g_ix_ry.growTo(sz);
    SubGraph<GrPerm> gy_excl(gy, 0);
    for (vert_id s : gy_excl.verts()) {
        for (vert_id d : gy_excl.succs(s)) {
            typename graph_t::mut_val_ref_t ws;
            typename graph_t::mut_val_ref_t wd;
            if (gx.lookup(s, 0, &ws) && gx.lookup(0, d, &wd)) {
                g_ix_ry.add_edge(s, ws.get() + wd.get(), d);
            }
        }
    }
    // Apply the deferred relations, and re-close.
    edge_vector delta;
    bool is_closed;
    graph_t g_rx(GrOps::meet(gx, g_ix_ry, is_closed));
    if (!is_closed) {
        SubGraph<graph_t> g_rx_excl(g_rx, 0);
        GrOps::close_after_meet(g_rx_excl, pot_rx, gx, g_ix_ry, delta);
        GrOps::apply_delta(g_rx, delta);
    }

    graph_t g_rx_iy;
    g_rx_iy.growTo(sz);

    SubGraph<GrPerm> gx_excl(gx, 0);
    for (vert_id s : gx_excl.verts()) {
        for (vert_id d : gx_excl.succs(s)) {
            typename graph_t::mut_val_ref_t ws;
            typename graph_t::mut_val_ref_t wd;
            // Assumption: gx.mem(s, d) -> gx.edge_val(s, d) <= ranges[var(s)].ub() - ranges[var(d)].lb()
            // That is, if the relation exists, it's at least as strong as the bounds.
            if (gy.lookup(s, 0, &ws) && gy.lookup(0, d, &wd))
                g_rx_iy.add_edge(s, ws.get() + wd.get(), d);
        }
    }
    delta.clear();
    // Similarly, should use a SubGraph view.
    graph_t g_ry(GrOps::meet(gy, g_rx_iy, is_closed));
    if (!is_closed) {

        SubGraph<graph_t> g_ry_excl(g_ry, 0);
        GrOps::close_after_meet(g_ry_excl, pot_ry, gy, g_rx_iy, delta);
        GrOps::apply_delta(g_ry, delta);
    }

    // We now have the relevant set of relations. Because g_rx and g_ry are closed,
    // the result is also closed.
    graph_t join_g(GrOps::join(g_rx, g_ry));

    // Now reapply the missing independent relations.
    // Need to derive vert_ids from lb_up/lb_down, and make sure the vertices exist
    std::vector<vert_id> lb_up;
    std::vector<vert_id> lb_down;
    std::vector<vert_id> ub_up;
    std::vector<vert_id> ub_down;

    typename graph_t::mut_val_ref_t wx;
    typename graph_t::mut_val_ref_t wy;
    for (vert_id v : gx_excl.verts()) {
        if (gx.lookup(0, v, &wx) && gy.lookup(0, v, &wy)) {
            if (wx.get() < wy.get())
                ub_up.push_back(v);
            if (wy.get() < wx.get())
                ub_down.push_back(v);
        }
        if (gx.lookup(v, 0, &wx) && gy.lookup(v, 0, &wy)) {
            if (wx.get() < wy.get())
                lb_down.push_back(v);
            if (wy.get() < wx.get())
                lb_up.push_back(v);
        }
    }

    for (vert_id s : lb_up) {
        Wt dx_s = gx.edge_val(s, 0);
        Wt dy_s = gy.edge_val(s, 0);
        for (vert_id d : ub_up) {
            if (s == d)
                continue;

            join_g.update_edge(s, std::max(dx_s + gx.edge_val(0, d), dy_s + gy.edge_val(0, d)), d);
        }
    }

    for (vert_id s : lb_down) {
        Wt dx_s = gx.edge_val(s, 0);
        Wt dy_s = gy.edge_val(s, 0);
        for (vert_id d : ub_down) {
            if (s == d)
                continue;

            join_g.update_edge(s, std::max(dx_s + gx.edge_val(0, d), dy_s + gy.edge_val(0, d)), d);
        }
    }

    // Conjecture: join_g remains closed.

    // Now garbage collect any unused vertices
    for (vert_id v : join_g.verts()) {
        if (v == 0)
            continue;
        if (join_g.succs(v).size() == 0 && join_g.preds(v).size() == 0) {
            join_g.forget(v);
            if (out_revmap[v]) {
                out_vmap.erase(*(out_revmap[v]));
                out_revmap[v] = std::nullopt;
            }
        }
    }

    // SplitDBM res(join_range, out_vmap, out_revmap, join_g, join_pot);
    SplitDBM res(std::move(out_vmap), std::move(out_revmap), std::move(join_g), std::move(pot_rx), vert_set_t());
    // join_g.check_adjs();
    CRAB_LOG("zones-split", std::cout << "Result join:\n" << res << "\n");

    return res;
}

SplitDBM SplitDBM::widen(SplitDBM o) {
    CrabStats::count("SplitDBM.count.widening");
    ScopedCrabStats __st__("SplitDBM.widening");

    if (is_bottom())
        return o;
    else if (o.is_bottom())
        return *this;
    else {
        CRAB_LOG("zones-split", std::cout << "Before widening:\n"
                                          << "DBM 1\n"
                                          << *this << "\n"
                                          << "DBM 2\n"
                                          << o << "\n");
        o.normalize();

        // Figure out the common renaming
        std::vector<vert_id> perm_x;
        std::vector<vert_id> perm_y;
        vert_map_t out_vmap;
        rev_map_t out_revmap;
        std::vector<Wt> widen_pot;
        vert_set_t widen_unstable(unstable);

        assert(!potential.empty());
        widen_pot.emplace_back(0);
        perm_x.push_back(0);
        perm_y.push_back(0);
        out_revmap.push_back(std::nullopt);
        for (auto [v, n] : vert_map) {
            auto it = o.vert_map.find(v);
            // Variable exists in both
            if (it != o.vert_map.end()) {
                out_vmap.insert(vmap_elt_t(v, static_cast<vert_id>(perm_x.size())));
                out_revmap.push_back(v);

                widen_pot.push_back(potential[n] - potential[0]);
                perm_x.push_back(n);
                perm_y.push_back(it->second);
            }
        }

        // Build the permuted view of x and y.
        assert(g.size() > 0);
        GrPerm gx(perm_x, g);
        assert(o.g.size() > 0);
        GrPerm gy(perm_y, o.g);

        // Now perform the widening
        std::vector<vert_id> destabilized;
        graph_t widen_g(GrOps::widen(gx, gy, destabilized));
        for (vert_id v : destabilized)
            widen_unstable.insert(v);

        SplitDBM res(std::move(out_vmap), std::move(out_revmap), std::move(widen_g), std::move(widen_pot),
                     std::move(widen_unstable));

        CRAB_LOG("zones-split", std::cout << "Result widening:\n" << res << "\n");
        return res;
    }
}
SplitDBM SplitDBM::operator&(SplitDBM o) {
    CrabStats::count("SplitDBM.count.meet");
    ScopedCrabStats __st__("SplitDBM.meet");

    if (is_bottom() || o.is_bottom())
        return SplitDBM::bottom();
    else if (is_top())
        return o;
    else if (o.is_top())
        return *this;
    else {
        CRAB_LOG("zones-split", std::cout << "Before meet:\n"
                                          << "DBM 1\n"
                                          << *this << "\n"
                                          << "DBM 2\n"
                                          << o << "\n");
        normalize();
        o.normalize();

        // We map vertices in the left operand onto a contiguous range.
        // This will often be the identity map, but there might be gaps.
        vert_map_t meet_verts;
        rev_map_t meet_rev;

        std::vector<vert_id> perm_x;
        std::vector<vert_id> perm_y;
        std::vector<Wt> meet_pi;
        perm_x.push_back(0);
        perm_y.push_back(0);
        meet_pi.emplace_back(0);
        meet_rev.push_back(std::nullopt);
        for (auto [v, n] : vert_map) {
            vert_id vv = static_cast<vert_id>(perm_x.size());
            meet_verts.insert(vmap_elt_t(v, vv));
            meet_rev.push_back(v);

            perm_x.push_back(n);
            perm_y.push_back(-1);
            meet_pi.push_back(potential[n] - potential[0]);
        }

        // Add missing mappings from the right operand.
        for (auto [v, n] : o.vert_map) {
            auto it = meet_verts.find(v);

            if (it == meet_verts.end()) {
                vert_id vv = static_cast<vert_id>(perm_y.size());
                meet_rev.push_back(v);

                perm_y.push_back(n);
                perm_x.push_back(-1);
                meet_pi.push_back(o.potential[n] - o.potential[0]);
                meet_verts.insert(vmap_elt_t(v, vv));
            } else {
                perm_y[it->second] = n;
            }
        }

        // Build the permuted view of x and y.
        assert(g.size() > 0);
        GrPerm gx(perm_x, g);
        assert(o.g.size() > 0);
        GrPerm gy(perm_y, o.g);

        // Compute the syntactic meet of the permuted graphs.
        bool is_closed;
        graph_t meet_g(GrOps::meet(gx, gy, is_closed));

        // Compute updated potentials on the zero-enriched graph
        // vector<Wt> meet_pi(meet_g.size());
        // We've warm-started pi with the operand potentials
        if (!GrOps::select_potentials(meet_g, meet_pi)) {
            // Potentials cannot be selected -- state is infeasible.
            return SplitDBM::bottom();
        }

        if (!is_closed) {
            edge_vector delta;
            SubGraph<graph_t> meet_g_excl(meet_g, 0);
            // GrOps::close_after_meet(meet_g_excl, meet_pi, gx, gy, delta);

            GrOps::close_after_meet(meet_g_excl, meet_pi, gx, gy, delta);

            GrOps::apply_delta(meet_g, delta);

            // Recover updated LBs and UBs.<

            delta.clear();
            GrOps::close_after_assign(meet_g, meet_pi, 0, delta);
            GrOps::apply_delta(meet_g, delta);
        }
        SplitDBM res(std::move(meet_verts), std::move(meet_rev), std::move(meet_g), std::move(meet_pi), vert_set_t());
        CRAB_LOG("zones-split", std::cout << "Result meet:\n" << res << "\n");
        return res;
    }
}

void SplitDBM::operator-=(variable_t v) {
    if (is_bottom())
        return;
    normalize();

    auto it = vert_map.find(v);
    if (it != vert_map.end()) {
        g.forget(it->second);
        rev_map[it->second] = std::nullopt;
        vert_map.erase(v);
    }
}

void SplitDBM::operator+=(const linear_constraint_t& cst) {
    CrabStats::count("SplitDBM.count.add_constraints");
    ScopedCrabStats __st__("SplitDBM.add_constraints");

    // XXX: we do nothing with unsigned linear inequalities
    if (cst.is_inequality() && cst.is_unsigned()) {
        CRAB_WARN("unsigned inequality ", cst, " skipped by split_dbm domain");
        return;
    }

    if (is_bottom())
        return;
    normalize();

    if (cst.is_tautology())
        return;

    // g.check_adjs();

    if (cst.is_contradiction()) {
        set_to_bottom();
        return;
    }

    if (cst.is_inequality()) {
        if (!add_linear_leq(cst.expression())) {
            set_to_bottom();
        }
        //  g.check_adjs();
        CRAB_LOG("zones-split", std::cout << "--- " << cst << "\n" << *this << "\n");
        return;
    }

    if (cst.is_strict_inequality()) {
        // We try to convert a strict to non-strict.
        // e < 0 --> e <= -1
        auto nc = linear_constraint_t(cst.expression() + 1, cst_kind::INEQUALITY, cst.is_signed());
        if (nc.is_inequality()) {
            // here we succeed
            if (!add_linear_leq(nc.expression())) {
                set_to_bottom();
            }
            CRAB_LOG("zones-split", std::cout << "--- " << cst << "\n" << *this << "\n");
            return;
        }
    }

    if (cst.is_equality()) {
        const linear_expression_t& exp = cst.expression();
        if (!add_linear_leq(exp) || !add_linear_leq(-exp)) {
            CRAB_LOG("zones-split", std::cout << " ~~> _|_"
                                              << "\n");
            set_to_bottom();
        }
        // g.check_adjs();
        CRAB_LOG("zones-split", std::cout << "--- " << cst << "\n" << *this << "\n");
        return;
    }

    if (cst.is_disequation()) {
        add_disequation(cst.expression());
        return;
    }

    CRAB_WARN("Unhandled constraint ", cst, " by split_dbm");
    CRAB_LOG("zones-split", std::cout << "---" << cst << "\n" << *this << "\n");
}

void SplitDBM::assign(variable_t x, const linear_expression_t& e) {
    CrabStats::count("SplitDBM.count.assign");
    ScopedCrabStats __st__("SplitDBM.assign");

    if (is_bottom()) {
        return;
    }

    CRAB_LOG("zones-split", std::cout << "Before assign: " << *this << "\n");
    CRAB_LOG("zones-split", std::cout << x << ":=" << e << "\n");
    normalize();

    interval_t x_int = eval_interval(e);

    std::optional<Wt> lb_w, ub_w;
    bool overflow{};
    if (x_int.lb().is_finite()) {
        lb_w = convert_NtoW(-(*(x_int.lb().number())), overflow);
        if (overflow) {
            operator-=(x);
            CRAB_LOG("zones-split", std::cout << "---" << x << ":=" << e << "\n" << *this << "\n");
            return;
        }
    }
    if (x_int.ub().is_finite()) {
        ub_w = convert_NtoW(*(x_int.ub().number()), overflow);
        if (overflow) {
            operator-=(x);
            CRAB_LOG("zones-split", std::cout << "---" << x << ":=" << e << "\n" << *this << "\n");
            return;
        }
    }

    bool is_rhs_constant = false;
    // If it's a constant, just assign the interval.

    // JN: it seems that we can only do this if
    // close_bounds_inline is disabled. Otherwise, the meet
    // operator misses some non-redundant edges. Need to
    // investigate more this.
    if (std::optional<number_t> x_n = x_int.singleton()) {
        set(x, interval_t{*x_n});
        is_rhs_constant = true;
    }

    if (!is_rhs_constant) {
        std::vector<std::pair<variable_t, Wt>> diffs_lb, diffs_ub;
        // Construct difference constraints from the assignment
        diffcsts_of_assign(x, e, diffs_lb, diffs_ub);
        if (!diffs_lb.empty() || !diffs_ub.empty()) {
            Wt e_val = eval_expression(e, overflow);
            if (overflow) {
                operator-=(x);
                return;
            }
            // Allocate a new vertex for x
            vert_id vert = g.new_vertex();
            assert(vert <= rev_map.size());
            if (vert == rev_map.size()) {
                rev_map.push_back(x);
                potential.push_back(potential[0] + e_val);
            } else {
                potential[vert] = potential[0] + e_val;
                rev_map[vert] = x;
            }

            edge_vector delta;
            for (auto [var, n] : diffs_lb) {
                delta.push_back({{vert, get_vert(var)}, -n});
            }

            for (auto [var, n] : diffs_ub) {
                delta.push_back({{get_vert(var), vert}, n});
            }

            // apply_delta should be safe here, as x has no edges in G.
            GrOps::apply_delta(g, delta);
            delta.clear();
            SubGraph<graph_t> g_excl(g, 0);
            GrOps::close_after_assign(g_excl, potential, vert, delta);
            GrOps::apply_delta(g, delta);

            if (lb_w) {
                g.update_edge(vert, *lb_w, 0);
            }
            if (ub_w) {
                g.update_edge(0, *ub_w, vert);
            }
            // Clear the old x vertex
            operator-=(x);
            vert_map.insert(vmap_elt_t(x, vert));
        } else {
            set(x, x_int);
        }
    }

    // CRAB_WARN("DBM only supports a cst or var on the rhs of assignment");
    // this->operator-=(x);
    // g.check_adjs();
    CRAB_LOG("zones-split", std::cout << "---" << x << ":=" << e << "\n" << *this << "\n");
}

void SplitDBM::rename(const variable_vector_t& from, const variable_vector_t& to) {
    CrabStats::count("SplitDBM.count.rename");
    ScopedCrabStats __st__("SplitDBM.rename");

    if (is_top() || is_bottom())
        return;

    // renaming vert_map by creating a new vert_map since we are
    // modifying the keys.
    // rev_map is modified in-place since we only modify values.
    CRAB_LOG("zones-split", std::cout << "Replacing {"; for (auto v
                                                             : from) std::cout
                                                        << v << ";";
             std::cout << "} with "; for (auto v
                                          : to) std::cout
                                     << v << ";";
             std::cout << "}:\n"; std::cout << *this << "\n";);

    vert_map_t new_vert_map;
    for (auto kv : vert_map) {
        ptrdiff_t pos = std::distance(from.begin(), std::find(from.begin(), from.end(), kv.first));
        if ((long unsigned)pos < from.size()) {
            variable_t new_v(to[pos]);
            new_vert_map.insert(vmap_elt_t(new_v, kv.second));
            rev_map[kv.second] = new_v;
        } else {
            new_vert_map.insert(kv);
        }
    }
    std::swap(vert_map, new_vert_map);

    CRAB_LOG("zones-split", std::cout << "RESULT=" << *this << "\n");
}

SplitDBM SplitDBM::narrow(SplitDBM o) {
    CrabStats::count("SplitDBM.count.narrowing");
    ScopedCrabStats __st__("SplitDBM.narrowing");

    if (is_bottom() || o.is_bottom())
        return SplitDBM::bottom();
    else if (is_top())
        return o;
    else {
        CRAB_LOG("zones-split", std::cout << "Before narrowing:\n"
                                          << "DBM 1\n"
                                          << *this << "\n"
                                          << "DBM 2\n"
                                          << o << "\n");

        // FIXME: Implement properly
        // Narrowing as a no-op should be sound.
        normalize();
        SplitDBM res(*this);

        CRAB_LOG("zones-split", std::cout << "Result narrowing:\n" << res << "\n");
        return res;
    }
}

void SplitDBM::normalize() {
    CrabStats::count("SplitDBM.count.normalize");
    ScopedCrabStats __st__("SplitDBM.normalize");

    // dbm_canonical(_dbm);
    // Always maintained in normal form, except for widening
    if (unstable.empty())
        return;

    edge_vector delta;
    // GrOps::close_after_widen(g, potential, vert_set_wrap_t(unstable), delta);
    // GKG: Check
    SubGraph<graph_t> g_excl(g, 0);
    GrOps::close_after_widen(g_excl, potential, vert_set_wrap_t(unstable), delta);
    // Retrive variable bounds
    GrOps::close_after_assign(g, potential, 0, delta);

    GrOps::apply_delta(g, delta);

    unstable.clear();
}

void SplitDBM::set(variable_t x, const interval_t& intv) {
    CrabStats::count("SplitDBM.count.assign");
    ScopedCrabStats __st__("SplitDBM.assign");

    if (is_bottom())
        return;

    if (intv.is_bottom()) {
        set_to_bottom();
        return;
    }

    this->operator-=(x);

    if (intv.is_top()) {
        return;
    }

    vert_id v = get_vert(x);
    bool overflow;
    if (intv.ub().is_finite()) {
        Wt ub = convert_NtoW(*(intv.ub().number()), overflow);
        if (overflow) {
            return;
        }
        potential[v] = potential[0] + ub;
        g.set_edge(0, ub, v);
    }
    if (intv.lb().is_finite()) {
        Wt lb = convert_NtoW(*(intv.lb().number()), overflow);
        if (overflow) {
            return;
        }
        potential[v] = potential[0] + lb;
        g.set_edge(v, -lb, 0);
    }
}

void SplitDBM::apply(arith_binop_t op, variable_t x, variable_t y, variable_t z) {
    CrabStats::count("SplitDBM.count.apply");
    ScopedCrabStats __st__("SplitDBM.apply");

    if (is_bottom()) {
        return;
    }

    normalize();

    switch (op) {
    case arith_binop_t::ADD: assign(x, var_add(y, z)); return;
    case arith_binop_t::SUB: assign(x, var_sub(y, z)); return;
    // For the rest of operations, we fall back on intervals.
    case arith_binop_t::MUL: set(x, get_interval(y) * get_interval(z)); break;
    case arith_binop_t::SDIV: set(x, get_interval(y) / get_interval(z)); break;
    case arith_binop_t::UDIV: set(x, get_interval(y).UDiv(get_interval(z))); break;
    case arith_binop_t::SREM: set(x, get_interval(y).SRem(get_interval(z))); break;
    case arith_binop_t::UREM: set(x, get_interval(y).URem(get_interval(z))); break;
    default: CRAB_ERROR("DBM: unreachable");
    }
}

void SplitDBM::apply(arith_binop_t op, variable_t x, variable_t y, const number_t& k) {
    CrabStats::count("SplitDBM.count.apply");
    ScopedCrabStats __st__("SplitDBM.apply");

    if (is_bottom()) {
        return;
    }

    normalize();

    switch (op) {
    case arith_binop_t::ADD: assign(x, var_add(y, k)); return;
    case arith_binop_t::SUB: assign(x, var_sub(y, k)); return;
    case arith_binop_t::MUL: assign(x, var_mul(k, y)); return;
    // For the rest of operations, we fall back on intervals.
    case arith_binop_t::SDIV: set(x, get_interval(y) / interval_t(k)); break;
    case arith_binop_t::UDIV: set(x, get_interval(y).UDiv(interval_t(k))); break;
    case arith_binop_t::SREM: set(x, get_interval(y).SRem(interval_t(k))); break;
    case arith_binop_t::UREM: set(x, get_interval(y).URem(interval_t(k))); break;
    default: CRAB_ERROR("DBM: unreachable");
    }
}

void SplitDBM::apply(bitwise_binop_t op, variable_t x, variable_t y, variable_t z) {
    CrabStats::count("SplitDBM.count.apply");
    ScopedCrabStats __st__("SplitDBM.apply");

    // Convert to intervals and perform the operation
    normalize();
    this->operator-=(x);

    interval_t yi = operator[](y);
    interval_t zi = operator[](z);
    interval_t xi = interval_t::bottom();
    switch (op) {
    case bitwise_binop_t::AND: xi = yi.And(zi); break;
    case bitwise_binop_t::OR: xi = yi.Or(zi); break;
    case bitwise_binop_t::XOR: xi = yi.Xor(zi); break;
    case bitwise_binop_t::SHL: xi = yi.Shl(zi); break;
    case bitwise_binop_t::LSHR: xi = yi.LShr(zi); break;
    case bitwise_binop_t::ASHR: xi = yi.AShr(zi); break;
    default: CRAB_ERROR("DBM: unreachable");
    }
    set(x, xi);
}

void SplitDBM::apply(bitwise_binop_t op, variable_t x, variable_t y, const number_t& k) {
    CrabStats::count("SplitDBM.count.apply");
    ScopedCrabStats __st__("SplitDBM.apply");

    // Convert to intervals and perform the operation
    normalize();
    interval_t yi = operator[](y);
    interval_t zi(k);
    interval_t xi = interval_t::bottom();

    switch (op) {
    case bitwise_binop_t::AND: xi = yi.And(zi); break;
    case bitwise_binop_t::OR: xi = yi.Or(zi); break;
    case bitwise_binop_t::XOR: xi = yi.Xor(zi); break;
    case bitwise_binop_t::SHL: xi = yi.Shl(zi); break;
    case bitwise_binop_t::LSHR: xi = yi.LShr(zi); break;
    case bitwise_binop_t::ASHR: xi = yi.AShr(zi); break;
    default: CRAB_ERROR("DBM: unreachable");
    }
    set(x, xi);
}

void SplitDBM::forget(const variable_vector_t& variables) {
    if (is_bottom() || is_top()) {
        return;
    }

    for (auto v : variables) {
        auto it = vert_map.find(v);
        if (it != vert_map.end()) {
            operator-=(v);
        }
    }
}

std::ostream& operator<<(std::ostream& o, SplitDBM& dom) {

    dom.normalize();

    if (dom.is_bottom()) {
        return o << "_|_";
    }
    if (dom.is_top()) {
        return o << "{}";
    }
    // Intervals
    bool first = true;
    o << "{";
    // Extract all the edges
    SubGraph<SplitDBM::graph_t> g_excl(dom.g, 0);
    for (SplitDBM::vert_id v : g_excl.verts()) {
        if (!dom.rev_map[v])
            continue;
        if (!dom.g.elem(0, v) && !dom.g.elem(v, 0))
            continue;
        interval_t v_out = interval_t(dom.g.elem(v, 0) ? -number_t(dom.g.edge_val(v, 0)) : bound_t::minus_infinity(),
                                      dom.g.elem(0, v) ?  number_t(dom.g.edge_val(0, v)) : bound_t::plus_infinity());

        if (first)
            first = false;
        else
            o << ", ";
        o << *(dom.rev_map[v]) << " -> ";
        if (v_out.lb() == v_out.ub())
            o << "[" << v_out.lb() << "]";
        else
            o << v_out;
    }
    if (!first) o << "\n ";
    first = true;

    for (SplitDBM::vert_id s : g_excl.verts()) {
        if (!dom.rev_map[s])
            continue;
        variable_t vs = *dom.rev_map[s];
        for (SplitDBM::vert_id d : g_excl.succs(s)) {
            if (!dom.rev_map[d])
                continue;
            variable_t vd = *dom.rev_map[d];

            if (first)
                first = false;
            else
                o << ", ";
            o << vd << "-" << vs << "<=" << g_excl.edge_val(s, d);
        }
    }
    o << "}";
    return o;
}

} // namespace crab::domains
