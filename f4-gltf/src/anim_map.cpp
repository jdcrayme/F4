// f4-gltf/src/anim_map.cpp
//
// Animation map construction + tagged-node local transform evaluation.
// Pure glTF document math — no rendering types, no I/O (the document
// must already be loaded). See anim_map.hpp for the contract.

#include <f4/gltf/anim_map.hpp>

#include <f4/anim/channels.hpp>

#include <cmath>
#include <set>

namespace f4::gltf {

namespace {

constexpr double kEps = 1e-12;

// ── quaternion helpers (double, [x,y,z,w] storage) ────────────────────────

void quat_identity(double q[4]) {
    q[0] = 0; q[1] = 0; q[2] = 0; q[3] = 1;
}

void quat_mul(const double a[4], const double b[4], double out[4]) {
    // Hamilton product a * b.
    out[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    out[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    out[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    out[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

void quat_normalize(double q[4]) {
    const double n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] +
                               q[3] * q[3]);
    if (n < kEps) {
        quat_identity(q);
        return;
    }
    q[0] /= n; q[1] /= n; q[2] /= n; q[3] /= n;
}

void quat_from_axis_angle(const double axis[3], double angle_rad,
                          double out[4]) {
    double ax[3] = {axis[0], axis[1], axis[2]};
    const double len = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
    if (len < kEps) {
        quat_identity(out);
        return;
    }
    ax[0] /= len; ax[1] /= len; ax[2] /= len;
    const double half = angle_rad * 0.5;
    const double s = std::sin(half);
    out[0] = ax[0] * s;
    out[1] = ax[1] * s;
    out[2] = ax[2] * s;
    out[3] = std::cos(half);
}

// ── node authored transform (matrix or TRS → t/q/s) ──────────────────────

/// Decompose a 4x4 column-major matrix into translation / rotation /
/// scale. Standard basis extraction — sufficient for the affine
/// transforms the emitter writes (rigid frames + axis-aligned scales;
/// no shear).
void decompose_matrix(const std::array<double, 16>& m, double t[3],
                      double q[4], double s[3]) {
    t[0] = m[12]; t[1] = m[13]; t[2] = m[14];

    // Column lengths = scale.
    const double sx = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
    const double sy = std::sqrt(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]);
    const double sz = std::sqrt(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);
    s[0] = sx; s[1] = sy; s[2] = sz;

    // Rotation from normalized column vectors.
    const double r[3][3] = {
        {m[0] / (sx < kEps ? 1.0 : sx), m[4] / (sy < kEps ? 1.0 : sy),
         m[8] / (sz < kEps ? 1.0 : sz)},
        {m[1] / (sx < kEps ? 1.0 : sx), m[5] / (sy < kEps ? 1.0 : sy),
         m[9] / (sz < kEps ? 1.0 : sz)},
        {m[2] / (sx < kEps ? 1.0 : sx), m[6] / (sy < kEps ? 1.0 : sy),
         m[10] / (sz < kEps ? 1.0 : sz)},
    };

    // Shear-free trace method (Shepperd's equations, standard form).
    const double trace = r[0][0] + r[1][1] + r[2][2];
    if (trace > 0.0) {
        const double S = std::sqrt(trace + 1.0) * 2.0;
        q[3] = 0.25 * S;
        q[0] = (r[2][1] - r[1][2]) / S;
        q[1] = (r[0][2] - r[2][0]) / S;
        q[2] = (r[1][0] - r[0][1]) / S;
    } else if (r[0][0] > r[1][1] && r[0][0] > r[2][2]) {
        const double S = std::sqrt(1.0 + r[0][0] - r[1][1] - r[2][2]) * 2.0;
        q[3] = (r[2][1] - r[1][2]) / S;
        q[0] = 0.25 * S;
        q[1] = (r[0][1] + r[1][0]) / S;
        q[2] = (r[0][2] + r[2][0]) / S;
    } else if (r[1][1] > r[2][2]) {
        const double S = std::sqrt(1.0 + r[1][1] - r[0][0] - r[2][2]) * 2.0;
        q[3] = (r[0][2] - r[2][0]) / S;
        q[0] = (r[0][1] + r[1][0]) / S;
        q[1] = 0.25 * S;
        q[2] = (r[1][2] + r[2][1]) / S;
    } else {
        const double S = std::sqrt(1.0 + r[2][2] - r[0][0] - r[1][1]) * 2.0;
        q[3] = (r[1][0] - r[0][1]) / S;
        q[0] = (r[0][2] + r[2][0]) / S;
        q[1] = (r[1][2] + r[2][1]) / S;
        q[2] = 0.25 * S;
    }
    quat_normalize(q);
}

/// Read a node's authored local transform as translation/quaternion/
/// scale, handling both the matrix and TRS encodings. Identity when
/// neither is present.
void node_authored_trs(const Node& node, double t[3], double q[4],
                       double s[3]) {
    t[0] = t[1] = t[2] = 0.0;
    quat_identity(q);
    s[0] = s[1] = s[2] = 1.0;

    if (node.matrix.has_value()) {
        decompose_matrix(*node.matrix, t, q, s);
        return;
    }
    if (node.translation.has_value()) {
        for (int i = 0; i < 3; ++i) t[i] = (*node.translation)[i];
    }
    if (node.rotation.has_value()) {
        for (int i = 0; i < 4; ++i) q[i] = (*node.rotation)[i];
        quat_normalize(q);
    }
    if (node.scale.has_value()) {
        for (int i = 0; i < 3; ++i) s[i] = (*node.scale)[i];
    }
}

} // namespace

// ── AnimMap ───────────────────────────────────────────────────────────────

std::vector<std::string> AnimMap::channels() const {
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const auto& n : nodes) {
        if (n.channel.empty()) continue;
        if (seen.insert(n.channel).second) out.push_back(n.channel);
    }
    return out;
}

const AnimNode* AnimMap::find_by_node(std::size_t node_index) const noexcept {
    for (const auto& n : nodes) {
        if (n.node_index == node_index) return &n;
    }
    return nullptr;
}

AnimMap build_anim_map(const GltfDocument& doc) {
    AnimMap map;

    // Depth-first walk of the default scene. `path` holds every node
    // index from the scene root down to the current node.
    std::vector<std::size_t> roots;
    if (doc.scene >= 0 && static_cast<std::size_t>(doc.scene) < doc.scenes.size()) {
        roots = doc.scenes[static_cast<std::size_t>(doc.scene)].nodes;
    } else if (!doc.scenes.empty()) {
        roots = doc.scenes.front().nodes;
    }

    // Iterative DFS with explicit stack of (node, path) — recursion is
    // fine too, but paths make depth explicit and the graphs are small.
    struct Frame {
        std::size_t node;
        std::size_t depth;
    };
    std::vector<Frame> stack;
    std::vector<std::size_t> path;

    for (const auto root : roots) {
        stack.push_back({root, 0});
        path.clear();

        while (!stack.empty()) {
            const auto fr = stack.back();
            stack.pop_back();

            // Unwind the path to this frame's depth, then push the node.
            path.resize(fr.depth);
            path.push_back(fr.node);

            const Node& node = doc.nodes[fr.node];
            if (node.has_f4) {
                const bool is_dof = (node.f4.kind == "dof");
                const bool is_sw_branch =
                    (node.f4.kind == "sw") && node.f4.sw_child.has_value() &&
                    *node.f4.sw_child >= 0;
                if (is_dof || is_sw_branch) {
                    AnimNode an;
                    an.node_index = fr.node;
                    an.kind = node.f4.kind;
                    an.id = node.f4.id;
                    an.channel = node.f4.channel.value_or(std::string{});
                    an.index = node.f4.dof_index.value_or(
                        node.f4.sw_index.value_or(-1));
                    if (is_sw_branch) an.sw_child = *node.f4.sw_child;
                    an.reversed = node.f4.reversed.value_or(false);
                    an.chain = path;
                    map.nodes.push_back(std::move(an));
                }
            }

            // Push children in REVERSE so the LIFO stack pops (and
            // therefore visits) them in document order — this keeps
            // AnimMap::channels() first-seen order deterministic and
            // matching the document layout.
            for (auto it = node.children.rbegin();
                 it != node.children.rend(); ++it) {
                if (*it < doc.nodes.size()) {
                    stack.push_back({*it, fr.depth + 1});
                }
            }
        }
    }

    return map;
}

bool has_animation_tags(const GltfDocument& doc) noexcept {
    // Same scene-reachability semantics as build_anim_map: only nodes
    // the default scene can reach count as animation tags. The legacy
    // flat layout emits dof/sw/slot stub nodes as ORPHANS beside the
    // geometry (the scene references root → lod nodes only) — those
    // stubs must not flip a flat document onto the animated draw path
    // (build_anim_map never sees them either).
    std::vector<std::size_t> roots;
    if (doc.scene >= 0 && static_cast<std::size_t>(doc.scene) < doc.scenes.size()) {
        roots = doc.scenes[static_cast<std::size_t>(doc.scene)].nodes;
    } else if (!doc.scenes.empty()) {
        roots = doc.scenes.front().nodes;
    }

    std::vector<std::size_t> stack(roots.begin(), roots.end());
    while (!stack.empty()) {
        const auto node_index = stack.back();
        stack.pop_back();
        if (node_index >= doc.nodes.size()) continue;
        const Node& node = doc.nodes[node_index];
        if (node.has_f4) {
            if (node.f4.kind == "dof") return true;
            if (node.f4.kind == "sw" && node.f4.sw_child.has_value()) {
                return true;
            }
        }
        for (const auto child : node.children) stack.push_back(child);
    }
    return false;
}

// ── eval_tagged_local ─────────────────────────────────────────────────────

void eval_tagged_local(const GltfDocument& doc, std::size_t node_index,
                       float raw_value, double out_translation[3],
                       double out_rotation[4], double out_scale[3]) {
    if (node_index >= doc.nodes.size()) {
        out_translation[0] = out_translation[1] = out_translation[2] = 0.0;
        quat_identity(out_rotation);
        out_scale[0] = out_scale[1] = out_scale[2] = 1.0;
        return;
    }

    const Node& node = doc.nodes[node_index];
    node_authored_trs(node, out_translation, out_rotation, out_scale);

    // Only "dof" nodes carry animated transforms; switch branches are
    // pure visibility toggles.
    if (!node.has_f4 || node.f4.kind != "dof") return;

    // FreeFalcon defaults when extras don't specify: rot about +X
    // (BDofNode rotates around local X — see geometry_extractor.cpp).
    double axis[3] = {1.0, 0.0, 0.0};
    std::string op = node.f4.op.value_or("rot");
    if (node.f4.axis.has_value()) {
        for (int i = 0; i < 3; ++i) axis[i] = (*node.f4.axis)[i];
    }

    const float a = f4::anim::process_dof_value(
        raw_value, node.f4.dof_flags.value_or(0),
        node.f4.dof_min.value_or(0.0f), node.f4.dof_max.value_or(0.0f),
        node.f4.dof_mult.value_or(1.0f));

    if (op == "trans") {
        // BTransNode::Draw: the translation vector scales by the
        // processed value; the authored frame translation stays.
        if (node.f4.trans.has_value()) {
            for (int i = 0; i < 3; ++i) {
                out_translation[i] += (*node.f4.trans)[i] * a;
            }
        }
        return;
    }

    if (op == "scale") {
        // BScaleNode::Draw: s = 1 - (1 - target) * a, composed with the
        // authored scale (authored is 1 for emitter-produced nodes).
        if (node.f4.scale_target.has_value()) {
            for (int i = 0; i < 3; ++i) {
                const double target = (*node.f4.scale_target)[i];
                const double factor = 1.0 - (1.0 - target) * a;
                out_scale[i] *= factor;
            }
        }
        return;
    }

    // op == "rot" (default): rotate about the local axis AFTER the
    // authored frame — FreeFalcon's R = dof_rotation * Rx(value).
    double q_op[4];
    quat_from_axis_angle(axis, static_cast<double>(a), q_op);
    double q_out[4];
    quat_mul(out_rotation, q_op, q_out);
    quat_normalize(q_out);
    for (int i = 0; i < 4; ++i) out_rotation[i] = q_out[i];
}

} // namespace f4::gltf
