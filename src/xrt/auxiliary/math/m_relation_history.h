// Copyright 2021, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Small utility for keeping track of the history of an xrt_space_relation, ie. for knowing where a HMD or
 * controller was in the past
 * @author Moshi Turner <moshiturner@protonmail.com>
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 * @author Korcan Hussein <korcan.hussein@collabora.com>
 * @ingroup aux_util
 */
#pragma once

#include "xrt/xrt_defines.h"

#include "math/m_filter_one_euro.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque type for storing the history of a space relation in a ring buffer
 *
 * @note Unlike the bare C++ data structure @ref HistoryBuffer this wraps, **this is a thread safe interface**,
 * and is safe for concurrent access from multiple threads.
 * (It is using a simple mutex, not a reader/writer lock, but that is fine until proven to be a bottleneck.)
 *
 * @ingroup aux_util
 */
struct m_relation_history;

/**
 * @brief Describes how the resulting space relation for the desired time stamp was generated.
 *
 * @relates m_relation_history
 */
enum m_relation_history_result
{
	M_RELATION_HISTORY_RESULT_INVALID = 0,       //!< The supplied timestamp was invalid (0) or buffer was empty
	M_RELATION_HISTORY_RESULT_EXACT,             //!< The exact desired timestamp was found
	M_RELATION_HISTORY_RESULT_INTERPOLATED,      //!< The desired timestamp was between two entries
	M_RELATION_HISTORY_RESULT_PREDICTED,         //!< The desired timestamp was newer than the most recent entry
	M_RELATION_HISTORY_RESULT_REVERSE_PREDICTED, //!< The desired timestamp was older than the oldest entry
};

struct m_relation_history_filters
{
	struct m_filter_euro_vec3 position;
	struct m_filter_euro_quat orientation;
};

/*!
 * Creates an opaque relation_history object.
 *
 * @public @memberof m_relation_history
 */
void
m_relation_history_create(struct m_relation_history **rh, struct m_relation_history_filters *motion_vector_filters);

/*!
 * Pushes a new pose to the history.
 *
 * If the history is full, it will also pop a pose out of the other side of the buffer.
 *
 * @return false if the timestamp is earlier than the most recent timestamp already recorded
 *
 * @public @memberof m_relation_history
 */
bool
m_relation_history_push(struct m_relation_history *rh, struct xrt_space_relation const *in_relation, int64_t timestamp);

/*!
 * Interpolates or extrapolates to the desired timestamp.
 *
 * Read-only operation - doesn't remove anything from the buffer or anything like that - you can call this as often as
 * you want.
 *
 * @public @memberof m_relation_history
 */
enum m_relation_history_result
m_relation_history_get(const struct m_relation_history *rh,
                       int64_t at_timestamp_ns,
                       struct xrt_space_relation *out_relation);

/*!
 * Estimates the movement (velocity and angular velocity) of a new relation based on
 * the latest relation found in the buffer (as returned by m_relation_history_get_latest).
 *
 * Read-only on m_relation_history and in_relation.
 * Copies in_relation->pose to out_relation->pose, and writes new flags and linear/angular velocities to
 * out_relation->pose. OK to alias in_relation and out_relation.
 *
 * @public @memberof m_relation_history
 */
bool
m_relation_history_estimate_motion(struct m_relation_history *rh,
                                   const struct xrt_space_relation *in_relation,
                                   int64_t timestamp,
                                   struct xrt_space_relation *out_relation);

/*!
 * Get the latest report in the buffer, if any.
 *
 * @param rh self
 * @param[out] out_time_ns Populated with the latest report time, if any
 * @param[out] out_relation Populated with the latest relation, if any
 *
 * @return false if the history is empty.
 *
 * @public @memberof m_relation_history
 */
bool
m_relation_history_get_latest(const struct m_relation_history *rh,
                              int64_t *out_time_ns,
                              struct xrt_space_relation *out_relation);

/*!
 * Returns the number of items in the history.
 *
 * @public @memberof m_relation_history
 */
uint32_t
m_relation_history_get_size(const struct m_relation_history *rh);

/*!
 * Clears the history from all of the items.
 *
 * @public @memberof m_relation_history
 */
void
m_relation_history_clear(struct m_relation_history *rh);

/*!
 * Destroys an opaque relation_history object.
 *
 * @public @memberof m_relation_history
 */
void
m_relation_history_destroy(struct m_relation_history **rh);

#ifdef __cplusplus
}
#endif


#ifdef __cplusplus
namespace xrt::auxiliary::math {

/*!
 * C++ interface for @ref m_relation_history, non-copyable/deletable.
 *
 * @ingroup aux_math
 */
class RelationHistory
{
public:
	/*!
	 * @copydoc m_relation_history_result
	 */
	typedef m_relation_history_result Result;


private:
	m_relation_history *mPtr{nullptr};


public:
	// clang-format off
	RelationHistory(struct m_relation_history_filters *motion_vector_filters) noexcept { m_relation_history_create(&mPtr, motion_vector_filters); }
	~RelationHistory() { m_relation_history_destroy(&mPtr); }
	// clang-format on

	// Special non-copyable reference.
	RelationHistory(RelationHistory const &) = delete;
	RelationHistory(RelationHistory &&) = delete;
	RelationHistory &
	operator=(RelationHistory const &) = delete;
	RelationHistory &
	operator=(RelationHistory &&) = delete;


	/*!
	 * @copydoc m_relation_history_push
	 */
	bool
	push(xrt_space_relation const &relation, int64_t ts) noexcept
	{
		return m_relation_history_push(mPtr, &relation, ts);
	}

	/*!
	 * @copydoc m_relation_history_get
	 */
	Result
	get(int64_t at_time_ns, xrt_space_relation *out_relation) const noexcept
	{
		return m_relation_history_get(mPtr, at_time_ns, out_relation);
	}

	/*!
	 * @copydoc m_relation_history_get_latest
	 */
	bool
	get_latest(int64_t *out_time_ns, xrt_space_relation *out_relation) const noexcept
	{
		return m_relation_history_get_latest(mPtr, out_time_ns, out_relation);
	}

	/*!
	 * @copydoc m_relation_history_get_size
	 */
	size_t
	size() const noexcept
	{
		return m_relation_history_get_size(mPtr);
	}

	/*!
	 * @copydoc m_relation_history_clear
	 */
	void
	clear() noexcept
	{
		return m_relation_history_clear(mPtr);
	}
};

} // namespace xrt::auxiliary::math
#endif
