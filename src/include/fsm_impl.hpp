
#ifndef __FSM_IMPL_HPP__
#define __FSM_IMPL_HPP__

#include <assert.h>
#include <unistd.h>
#include "event_queue.hpp"
#include "fsm.hpp"

// TODO: we should refactor the FSM to be able to unit test state
// transitions independant of the OS network subsystem (via mock-ups,
// or via turning the code 'inside-out' in a Haskell sense).

template<class io_calls_t, class alloc_t>
void fsm_state_t<io_calls_t, alloc_t>::init_state() {
    this->state = fsm_socket_connected;
    this->buf = NULL;
    this->nbuf = 0;
    this->snbuf = 0;
}

// This function returns the socket to clean connected state
template<class io_calls_t, class alloc_t>
void fsm_state_t<io_calls_t, alloc_t>::return_to_socket_connected() {
    alloc->free(this->buf);
    init_state();
}

// This state represents a connected socket with no outstanding
// operations. Incoming events should be user commands received by the
// socket.
template<class io_calls_t, class alloc_t>
int fsm_state_t<io_calls_t, alloc_t>::do_socket_ready(event_t *event) {
    int res;
    size_t sz;
    fsm_state_t *state = (fsm_state_t*)event->state;

    if(event->event_type == et_sock) {
        if(state->buf == NULL) {
            state->buf = (char*)alloc->malloc(IO_BUFFER_SIZE);
            state->nbuf = 0;
        }
            
        // TODO: we assume the command will fit comfortably into
        // IO_BUFFER_SIZE. We'll need to implement streaming later.

        do {
            sz = io_calls_t::read(state->source,
                                  state->buf + state->nbuf,
                                  IO_BUFFER_SIZE - state->nbuf);
            if(sz == -1) {
                if(errno == EAGAIN || errno == EWOULDBLOCK) {
                    // The machine can't be in
                    // fsm_socket_send_incomplete state here,
                    // since we break out in these cases. So it's
                    // safe to free the buffer.
                    if(state->state != fsm_state_t::fsm_socket_recv_incomplete)
                        return_to_socket_connected();
                    break;
                } else {
                    check("Could not read from socket", sz == -1);
                }
            } else if(sz > 0) {
                state->nbuf += sz;
                res = operations->process_command(event);
                if(res == operations_t::malformed_command ||
                   res == operations_t::command_success_no_response ||
                   res == operations_t::command_success_response_ready)
                {
                    if(res == operations_t::malformed_command) {
                        // Command wasn't processed correctly, send error
                        send_err_to_client();
                    } else if(res == operations_t::command_success_response_ready) {
                        // Command wasn't processed correctly, send error
                        send_msg_to_client();
                    }
                    if(state->state != fsm_state_t::fsm_socket_send_incomplete) {
                        // Command is either completed or malformed, in any
                        // case get back to clean connected state
                        state->state = fsm_state_t::fsm_socket_connected;
                        state->nbuf = 0;
                        state->snbuf = 0;
                    } else {
                        // Wait for the socket to finish sending
                        break;
                    }
                } else if(res == operations_t::incomplete_command) {
                    state->state = fsm_state_t::fsm_socket_recv_incomplete;
                } else if(res == operations_t::quit_connection) {
                    // The connection has been closed
                    return quit_connection;
                } else if(res == operations_t::shutdown_server) {
                    // Shutdown has been initiated
                    return shutdown_server;
                } else {
                    check("Unhandled result", 1);
                }
            } else {
                // Socket has been closed, destroy the connection
                return quit_connection;
                    
                // TODO: what if the fsm is not in a finished
                // state? What if we free it during an AIO
                // request, and the AIO request comes back? We
                // need an fsm_terminated flag for these cases.

                // TODO: what about application-level keepalive?
            }
        } while(1);
    } else {
        check("fsm_socket_ready: Invalid event", 1);
    }

    return transition_ok;
}

// The socket is ready for sending more information and we were in the
// middle of an incomplete send request.
template<class io_calls_t, class alloc_t>
int fsm_state_t<io_calls_t, alloc_t>::do_socket_send_incomplete(event_t *event) {
    // TODO: incomplete send needs to be tested therally. It's not
    // clear how to get the kernel to artifically limit the send
    // buffer.
    if(event->event_type == et_sock) {
        if(event->op == eo_rdwr || event->op == eo_write) {
            send_msg_to_client();
        }
        if(this->state != fsm_state_t::fsm_socket_send_incomplete) {
            // We've finished sending completely, now see if there is
            // anything left to read from the old epoll notification,
            // and let fsm_socket_ready do the cleanup
            event->op = eo_read;
            do_socket_ready(event);
        }
    } else {
        check("fsm_socket_send_ready: Invalid event", 1);
    }
    return 0;
}

// Switch on the current state and call the appropriate transition
// function.
template<class io_calls_t, class alloc_t>
int fsm_state_t<io_calls_t, alloc_t>::do_transition(event_t *event) {
    // TODO: Using parent_pool member variable within state
    // transitions might cause cache line alignment issues. Can we
    // eliminate it (perhaps by giving each thread its own private
    // copy of the necessary data)?

    int res;

    switch(state) {
    case fsm_state_t::fsm_socket_connected:
    case fsm_state_t::fsm_socket_recv_incomplete:
        res = do_socket_ready(event);
        break;
    case fsm_state_t::fsm_socket_send_incomplete:
        res = do_socket_send_incomplete(event);
        break;
    default:
        res = -1;
        check("Invalid state", 1);
    }

    return res;
}

template<class io_calls_t, class alloc_t>
fsm_state_t<io_calls_t, alloc_t>::fsm_state_t(resource_t _source, alloc_t* _alloc,
                                     operations_t *_ops)
    : event_state_t(_source), alloc(_alloc), operations(_ops)
{
    init_state();
}

template<class io_calls_t, class alloc_t>
fsm_state_t<io_calls_t, alloc_t>::~fsm_state_t() {
    if(this->buf) {
        alloc->free(this->buf);
    }
}

// Send a message to the client. The message should be contained
// within buf (nbuf should be the full size). If state has been
// switched to fsm_socket_send_incomplete, then buf must not be freed
// after the return of this function.
template<class io_calls_t, class alloc_t>
void fsm_state_t<io_calls_t, alloc_t>::send_msg_to_client() {
    // Either number of bytes already sent should be zero, or we
    // should be in the middle of an incomplete send.
    assert(this->snbuf == 0 || this->state == fsm_state_t::fsm_socket_send_incomplete);

    int len = this->nbuf - this->snbuf;
    int sz = 0;
    do {
        this->snbuf += sz;
        len -= sz;
        
        sz = io_calls_t::write(this->source, this->buf + this->snbuf, len);
        if(sz < 0) {
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                // If we can't send the message now, wait 'till we can
                this->state = fsm_state_t::fsm_socket_send_incomplete;
                return;
            } else {
                // There was some other error
                check("Couldn't send message to client", sz < 0);
            }
        }
    } while(sz < len);

    // We've successfully sent everything out
    this->snbuf = 0;
    this->nbuf = 0;
    this->state = fsm_state_t::fsm_socket_connected;
}

template<class io_calls_t, class alloc_t>
void fsm_state_t<io_calls_t, alloc_t>::send_err_to_client() {
    char err_msg[] = "(ERROR) Unknown command\n";
    strcpy(this->buf, err_msg);
    this->nbuf = strlen(err_msg) + 1;
    send_msg_to_client();
}

#endif // __FSM_IMPL_HPP__

