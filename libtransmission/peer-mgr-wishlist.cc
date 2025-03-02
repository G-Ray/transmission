// This file Copyright © 2021-2023 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <cstddef>
#include <set>
#include <utility>
#include <vector>

#define LIBTRANSMISSION_PEER_MODULE

#include "transmission.h"

#include "crypto-utils.h" // for tr_salt_shaker
#include "peer-mgr-wishlist.h"

namespace
{
using SaltType = tr_piece_index_t;

struct Candidate
{
    tr_piece_index_t piece;
    size_t n_blocks_missing;
    tr_priority_t priority;
    SaltType salt;

    Candidate(tr_piece_index_t piece_in, size_t missing_in, tr_priority_t priority_in, SaltType salt_in)
        : piece{ piece_in }
        , n_blocks_missing{ missing_in }
        , priority{ priority_in }
        , salt{ salt_in }
    {
    }

    [[nodiscard]] int compare(Candidate const& that) const // <=>
    {
        // prefer pieces closer to completion
        if (n_blocks_missing != that.n_blocks_missing)
        {
            return n_blocks_missing < that.n_blocks_missing ? -1 : 1;
        }

        // prefer higher priority
        if (priority != that.priority)
        {
            return priority > that.priority ? -1 : 1;
        }

        if (salt != that.salt)
        {
            return salt < that.salt ? -1 : 1;
        }

        return 0;
    }

    bool operator<(Candidate const& that) const // less than
    {
        return compare(that) < 0;
    }
};

std::vector<Candidate> getCandidates(Wishlist::Mediator const& mediator)
{
    // count up the pieces that we still want
    auto wanted_pieces = std::vector<std::pair<tr_piece_index_t, size_t>>{};
    auto const n_pieces = mediator.countAllPieces();
    wanted_pieces.reserve(n_pieces);
    for (tr_piece_index_t i = 0; i < n_pieces; ++i)
    {
        if (!mediator.clientCanRequestPiece(i))
        {
            continue;
        }

        size_t const n_missing = mediator.countMissingBlocks(i);
        if (n_missing == 0)
        {
            continue;
        }

        wanted_pieces.emplace_back(i, n_missing);
    }

    // transform them into candidates
    auto salter = tr_salt_shaker<SaltType>{};
    auto const n = std::size(wanted_pieces);
    auto candidates = std::vector<Candidate>{};
    auto const is_sequential = mediator.isSequentialDownload();
    auto const sequential_from_piece = mediator.sequentialDownloadFromPiece();
    candidates.reserve(n);

    // In sequential download mode, start downloading from a specific piece (e.g middle of a video)
    if (is_sequential && sequential_from_piece > 0 && sequential_from_piece < wanted_pieces.size())
    {
        tr_logAddInfo(fmt::format("rotating wanted_piece from piece {}", sequential_from_piece));
        std::rotate(wanted_pieces.begin(), wanted_pieces.begin() + sequential_from_piece, wanted_pieces.end());
    }

    for (size_t i = 0; i < n; ++i)
    {
        auto const [piece, n_missing] = wanted_pieces[i];
        auto const salt = is_sequential ? piece : salter();
        candidates.emplace_back(piece, n_missing, mediator.priority(piece), salt);
    }

    return candidates;
}

std::vector<tr_block_span_t> makeSpans(tr_block_index_t const* sorted_blocks, size_t n_blocks)
{
    if (n_blocks == 0)
    {
        return {};
    }

    auto spans = std::vector<tr_block_span_t>{};
    auto cur = tr_block_span_t{ sorted_blocks[0], sorted_blocks[0] + 1 };
    for (size_t i = 1; i < n_blocks; ++i)
    {
        if (cur.end == sorted_blocks[i])
        {
            ++cur.end;
        }
        else
        {
            spans.push_back(cur);
            cur = tr_block_span_t{ sorted_blocks[i], sorted_blocks[i] + 1 };
        }
    }
    spans.push_back(cur);

    return spans;
}

} // namespace

// Cancel slow request for `block` if the new peer is considered faster
void cancelSlowRequest(Wishlist::Mediator const& mediator, tr_block_index_t block, tr_peer const* peer)
{
    auto const now = tr_time();
    std::vector<std::pair<tr_peer*, time_t>> const peers = mediator.getPeersForActiveRequests(block);
    auto const peer_speed = peer->get_piece_speed_bytes_per_second(now, TR_PEER_TO_CLIENT);

    if (peer_speed == 0)
    {
        return;
    }

    for (auto const& [current_peer, when] : peers)
    {
        auto const current_peer_speed = current_peer->get_piece_speed_bytes_per_second(now, TR_PEER_TO_CLIENT);

        // Avoid division by zero.
        if (current_peer_speed == 0)
        {
            continue;
        }

        // Estimate if time to request the block with new peer will be faster than letting it finish
        int res = (peer_speed / current_peer_speed) - (((now - when) * peer_speed) / tr_block_info::BlockSize);
        bool is_slow = res > 1.5; // Consider it slow if it's a bit faster than estimated

        if (is_slow)
        {
            tr_logAddTrace(fmt::format("cancelling slow request to block {}", block));
            tr_cancelRequestForBlock(current_peer, block);
            return;
        }
    }
}

std::vector<tr_block_span_t> Wishlist::next(size_t n_wanted_blocks, tr_peer const* peer)
{
    if (n_wanted_blocks == 0)
    {
        return {};
    }

    auto candidates = getCandidates(mediator_);
    auto const is_sequential = mediator_.isSequentialDownload();

    if (!is_sequential)
    {
        // We usually won't need all the candidates to be sorted until endgame, so don't
        // waste cycles sorting all of them here. partial sort is enough.
        auto constexpr MaxSortedPieces = size_t{ 30 };
        auto const middle = std::min(std::size(candidates), MaxSortedPieces);
        std::partial_sort(std::begin(candidates), std::begin(candidates) + middle, std::end(candidates));
    }

    auto blocks = std::set<tr_block_index_t>{};

    for (auto const& candidate : candidates)
    {
        // do we have enough?
        if (std::size(blocks) >= n_wanted_blocks)
        {
            break;
        }

        // walk the blocks in this piece
        auto const [begin, end] = mediator_.blockSpan(candidate.piece);
        for (tr_block_index_t block = begin; block < end && std::size(blocks) < n_wanted_blocks; ++block)
        {
            // don't request blocks we've already got
            if (!mediator_.clientCanRequestBlock(block))
            {
                continue;
            }

            if (is_sequential && mediator_.countActiveRequests(block) > 0)
            {
                // In sequential download mode, we want to retrieve blocks as
                // fast as possible, so cancel existing request if peer is slow
                // and new one is faster
                cancelSlowRequest(mediator_, block, peer);
            }

            // don't request from too many peers
            size_t const n_peers = mediator_.countActiveRequests(block);
            if (size_t const max_peers = mediator_.isEndgame() ? 2 : 1; n_peers >= max_peers)
            {
                continue;
            }

            blocks.insert(block);
        }
    }

    auto const blocks_v = std::vector<tr_block_index_t>{ std::begin(blocks), std::end(blocks) };
    return makeSpans(std::data(blocks_v), std::size(blocks_v));
}
