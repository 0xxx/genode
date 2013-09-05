/*
 * \brief   End point of inter-process communication
 * \author  Martin Stein
 * \date    2012-11-30
 */

/*
 * Copyright (C) 2012-2013 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU General Public License version 2.
 */

#ifndef _KERNEL__IPC_NODE_H_
#define _KERNEL__IPC_NODE_H_

/* Genode includes */
#include <util/fifo.h>

/* core includes */
#include <assert.h>

namespace Kernel
{
	/**
	 * Sends requests to other IPC nodes, accumulates request announcments,
	 * provides serial access to them and replies to them if expected.
	 * Synchronizes communication.
	 *
	 * IPC node states:
	 *
	 *         +----------+                               +---------------+                             +---------------+
	 * --new-->| inactive |---send-request-await-reply--->| await reply   |               +--send-note--| prepare reply |
	 *         |          |<--receive-reply---------------|               |               |             |               |
	 *         |          |<--cancel-waiting--------------|               |               |             |               |
	 *         |          |                               +---------------+               +------------>|               |
	 *         |          |<--request-is-a-note-------+---request-is-not-a-note------------------------>|               |
	 *         |          |<--------------------------(---not-await-request---+                         |               |
	 *         |          |                           |   +---------------+   |                         |               |
	 *         |          |---await-request-----------+-->| await request |<--+--send-reply-------------|               |
	 *         |          |<--cancel-waiting--------------|               |------announce-request--+--->|               |
	 *         |          |---send-reply---------+----+-->|               |                        |    |               |
	 *         |          |---send-note--+       |    |   +---------------+                        |    |               |
	 *         |          |              |       |    |                                            |    |               |
	 *         |          |<-------------+       |  request available                              |    |               |
	 *         |          |<--not-await-request--+    |                                            |    |               |
	 *         |          |<--request-is-a-note-------+-------------------request-is-not-a-note----(--->|               |
	 *         |          |<--request-is-a-note----------------------------------------------------+    |               |
	 *         +----------+                 +-------------------------+                                 |               |
	 *                                      | prepare and await reply |<--send-request-and-await-reply--|               |
	 *                                      |                         |---receive-reply---------------->|               |
	 *                                      |                         |---cancel-waiting--------------->|               |
	 *                                      +-------------------------+                                 +---------------+
	 *
	 * State model propagated to deriving classes:
	 *
	 *         +--------------+                                               +----------------+
	 * --new-->| has received |--send-request-await-reply-------------------->| awaits receipt |
	 *         |              |--await-request----------------------------+-->|                |
	 *         |              |                                           |   |                |
	 *         |              |<--request-available-----------------------+   |                |
	 *         |              |--send-reply-------------------------------+-->|                |
	 *         |              |--send-note--+                             |   |                |
	 *         |              |             |                             |   |                |
	 *         |              |<------------+                             |   |                |
	 *         |              |<--request-available-or-not-await-request--+   |                |
	 *         |              |<--announce-request----------------------------|                |
	 *         |              |<--receive-reply-------------------------------|                |
	 *         |              |<--cancel-waiting------------------------------|                |
	 *         +--------------+                                               +----------------+
	 */
	class Ipc_node;
}

class Kernel::Ipc_node
{
	private:

		template <typename T> class Fifo : public Genode::Fifo<T> { };

		/**
		 * IPC node states as depicted initially
		 */
		enum State
		{
			INACTIVE = 1,
			AWAIT_REPLY = 2,
			AWAIT_REQUEST = 3,
			PREPARE_REPLY = 4,
			PREPARE_AND_AWAIT_REPLY = 5,
		};

		/**
		 * Describes the buffer for incoming or outgoing messages
		 */
		struct Message_buf : public Fifo<Message_buf>::Element
		{
			void * base;
			size_t size;
			Ipc_node * origin;
		};

		Fifo<Message_buf> _request_queue; /* requests that waits to be
		                                   * received by us */
		Message_buf _inbuf;  /* buffers message we have received lastly */
		Message_buf _outbuf; /* buffers the message we aim to send */
		State       _state;  /* current node state */

		/**
		 * Buffer next request from request queue in 'r' to handle it
		 */
		void _receive_request(Message_buf * const r)
		{
			/* FIXME: invalid requests should be discarded */
			if (r->size > _inbuf.size) {
				PERR("oversized request");
				while (1) { }
			}
			/* fetch message */
			Genode::memcpy(_inbuf.base, r->base, r->size);
			_inbuf.size = r->size;
			_inbuf.origin = r->origin;

			/* update state */
			_state = r->origin->_awaits_reply() ? PREPARE_REPLY :
			                                      INACTIVE;
		}

		/**
		 * Receive a given reply if one is expected
		 *
		 * \param base  base of the reply payload
		 * \param size  size of the reply payload
		 */
		void _receive_reply(void * const base, size_t const size)
		{
			/* FIXME: when discard awaited replies userland must get a hint */
			if (!_awaits_reply() || size > _inbuf.size) {
				PDBG("discard invalid IPC reply");
				return;
			}
			/* receive reply */
			Genode::memcpy(_inbuf.base, base, size);
			_inbuf.size = size;

			/* update state */
			if (_state != PREPARE_AND_AWAIT_REPLY) { _state = INACTIVE; }
			else { _state = PREPARE_REPLY; }
			_has_received(_inbuf.size);
		}

		/**
		 * Insert 'r' into request queue, buffer it if we were waiting for it
		 */
		void _announce_request(Message_buf * const r)
		{
			/* directly receive request if we've awaited it */
			if (_state == AWAIT_REQUEST) {
				_receive_request(r);
				_has_received(_inbuf.size);
				return;
			}
			/* cannot receive yet, so queue request */
			_request_queue.enqueue(r);
		}

		/**
		 * Wether we expect to receive a reply message
		 */
		bool _awaits_reply()
		{
			return _state == AWAIT_REPLY ||
			       _state == PREPARE_AND_AWAIT_REPLY;
		}

		/**
		 * IPC node waits for a message to receive to its inbuffer
		 */
		virtual void _awaits_receipt() = 0;

		/**
		 * IPC node has received a message in its inbuffer
		 *
		 * \param s  size of the message
		 */
		virtual void _has_received(size_t const s) = 0;

	public:

		/**
		 * Construct an initially inactive IPC node
		 */
		Ipc_node() : _state(INACTIVE)
		{
			_inbuf.size = 0;
			_outbuf.size = 0;
		}

		/**
		 * Destructor
		 */
		virtual ~Ipc_node() { }

		/**
		 * Send a request and wait for the according reply
		 *
		 * \param dest        targeted IPC node
		 * \param req_base    base of the request payload
		 * \param req_size    size of the request payload
		 * \param inbuf_base  base of the reply buffer
		 * \param inbuf_size  size of the reply buffer
		 */
		void send_request_await_reply(Ipc_node * const dest,
		                              void * const     req_base,
		                              size_t const     req_size,
		                              void * const     inbuf_base,
		                              size_t const     inbuf_size)
		{
			/* assertions */
			assert(_state == INACTIVE || _state == PREPARE_REPLY);

			/* prepare transmission of request message */
			_outbuf.base = req_base;
			_outbuf.size = req_size;
			_outbuf.origin = this;

			/* prepare reception of reply message */
			_inbuf.base = inbuf_base;
			_inbuf.size = inbuf_size;

			/* update state */
			if (_state != PREPARE_REPLY) { _state = AWAIT_REPLY; }
			else { _state = PREPARE_AND_AWAIT_REPLY; }
			_awaits_receipt();

			/* announce request */
			dest->_announce_request(&_outbuf);
		}

		/**
		 * Wait until a request has arrived and load it for handling
		 *
		 * \param inbuf_base  base of the request buffer
		 * \param inbuf_size  size of the request buffer
		 */
		void await_request(void * const inbuf_base,
		                   size_t const inbuf_size)
		{
			/* assertions */
			assert(_state == INACTIVE);

			/* prepare receipt of request */
			_inbuf.base = inbuf_base;
			_inbuf.size = inbuf_size;

			/* if anybody already announced a request receive it */
			if (!_request_queue.empty()) {
				_receive_request(_request_queue.dequeue());
				_has_received(_inbuf.size);
				return;
			}
			/* no request announced, so wait */
			_state = AWAIT_REQUEST;
			_awaits_receipt();
		}

		/**
		 * Reply to last request if there's any
		 *
		 * \param reply_base  base of the reply payload
		 * \param reply_size  size of the reply payload
		 */
		void send_reply(void * const reply_base,
		                size_t const reply_size)
		{
			/* reply to the last request if we have to */
			if (_state == PREPARE_REPLY) {
				_inbuf.origin->_receive_reply(reply_base, reply_size);
				_state = INACTIVE;
			}
		}

		/**
		 * Send a notification and stay inactive
		 *
		 * \param dest        targeted IPC node
		 * \param note_base   base of the note payload
		 * \param note_size   size of the note payload
		 *
		 * The caller must ensure that the note payload remains
		 * until it is buffered by the targeted node.
		 */
		void send_note(Ipc_node * const dest,
		               void * const     note_base,
		               size_t const     note_size)
		{
			/* assert preconditions */
			assert(_state == INACTIVE || _state == PREPARE_REPLY);

			/* announce request message, our state says: No reply needed */
			_outbuf.base = note_base;
			_outbuf.size = note_size;
			_outbuf.origin = this;
			dest->_announce_request(&_outbuf);
		}

		/**
		 * Stop waiting for a receipt if in a waiting state
		 */
		void cancel_waiting()
		{
			if (_state == PREPARE_AND_AWAIT_REPLY) { _state = PREPARE_REPLY; }
			if (_state == AWAIT_REPLY || _state == AWAIT_REQUEST) {
				_state = INACTIVE;
			}
		}
};

#endif /* _KERNEL__IPC_NODE_H_ */
