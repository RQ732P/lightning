#include <common/key_derive.h>
#include <errno.h>
#include <inttypes.h>
#include <lightningd/chaintopology.h>
#include <lightningd/lightningd.h>
#include <lightningd/log.h>
#include <lightningd/onchain_control.h>
#include <lightningd/peer_control.h>
#include <lightningd/subd.h>
#include <lightningd/watch.h>
#include <onchaind/gen_onchain_wire.h>
#include <onchaind/onchain_wire.h>

/* We dump all the known preimages when onchaind starts up. */
static void onchaind_tell_fulfill(struct channel *channel)
{
	struct htlc_in_map_iter ini;
	struct htlc_in *hin;
	u8 *msg;
	struct lightningd *ld = channel->peer->ld;

	for (hin = htlc_in_map_first(&ld->htlcs_in, &ini);
	     hin;
	     hin = htlc_in_map_next(&ld->htlcs_in, &ini)) {
		if (hin->key.channel != channel)
			continue;

		/* BOLT #5:
		 *
		 * If the node receives (or already knows) a payment preimage
		 * for an unresolved HTLC output it was offered for which it
		 * has committed to an outgoing HTLC, it MUST *resolve* the
		 * output by spending it.  Otherwise, if the other node is not
		 * irrevocably committed to the HTLC, it MUST NOT *resolve*
		 * the output by spending it.
		 */

		/* We only set preimage once it's irrevocably committed, and
		 * we spend even if we don't have an outgoing HTLC (eg. local
		 * payment complete) */
		if (!hin->preimage)
			continue;

		msg = towire_onchain_known_preimage(channel, hin->preimage);
		subd_send_msg(channel->owner, take(msg));
	}
}

static void handle_onchain_init_reply(struct channel *channel, const u8 *msg)
{
	u8 state;

	if (!fromwire_onchain_init_reply(msg, &state)) {
		channel_internal_error(channel, "Invalid onchain_init_reply");
		return;
	}

	if (!channel_state_on_chain(state)) {
		channel_internal_error(channel,
				    "Invalid onchain_init_reply state %u (%s)",
				    state, channel_state_str(state));
		return;
	}

	channel_set_state(channel, FUNDING_SPEND_SEEN, state);

	/* Tell it about any preimages we know. */
	onchaind_tell_fulfill(channel);
}

static enum watch_result onchain_tx_watched(struct channel *channel,
					    const struct bitcoin_tx *tx,
					    unsigned int depth)
{
	u8 *msg;
	struct bitcoin_txid txid;

	if (depth == 0) {
		log_unusual(channel->log, "Chain reorganization!");
		channel_set_owner(channel, NULL);

		/* FIXME!
		topology_rescan(peer->ld->topology, peer->funding_txid);
		*/

		/* We will most likely be freed, so this is a noop */
		return KEEP_WATCHING;
	}

	bitcoin_txid(tx, &txid);
	msg = towire_onchain_depth(channel, &txid, depth);
	subd_send_msg(channel->owner, take(msg));
	return KEEP_WATCHING;
}

static void watch_tx_and_outputs(struct channel *channel,
				 const struct bitcoin_tx *tx);

static enum watch_result onchain_txo_watched(struct channel *channel,
					     const struct bitcoin_tx *tx,
					     size_t input_num,
					     const struct block *block)
{
	u8 *msg;

	watch_tx_and_outputs(channel, tx);

	msg = towire_onchain_spent(channel, tx, input_num, block->height);
	subd_send_msg(channel->owner, take(msg));

	/* We don't need to keep watching: If this output is double-spent
	 * (reorg), we'll get a zero depth cb to onchain_tx_watched, and
	 * restart onchaind. */
	return DELETE_WATCH;
}

/* To avoid races, we watch the tx and all outputs. */
static void watch_tx_and_outputs(struct channel *channel,
				 const struct bitcoin_tx *tx)
{
	struct bitcoin_txid txid;
	struct txwatch *txw;
	struct lightningd *ld = channel->peer->ld;

	bitcoin_txid(tx, &txid);

	/* Make txwatch a parent of txo watches, so we can unwatch together. */
	txw = watch_tx(channel->owner, ld->topology, channel, tx,
		       onchain_tx_watched);

	for (size_t i = 0; i < tal_count(tx->output); i++)
		watch_txo(txw, ld->topology, channel, &txid, i,
			  onchain_txo_watched);
}

static void handle_onchain_broadcast_tx(struct channel *channel, const u8 *msg)
{
	struct bitcoin_tx *tx;

	if (!fromwire_onchain_broadcast_tx(msg, msg, &tx)) {
		channel_internal_error(channel, "Invalid onchain_broadcast_tx");
		return;
	}

	/* We don't really care if it fails, we'll respond via watch. */
	broadcast_tx(channel->peer->ld->topology, channel, tx, NULL);
}

static void handle_onchain_unwatch_tx(struct channel *channel, const u8 *msg)
{
	struct bitcoin_txid txid;
	struct txwatch *txw;

	if (!fromwire_onchain_unwatch_tx(msg, &txid)) {
		channel_internal_error(channel, "Invalid onchain_unwatch_tx");
		return;
	}

	/* Frees the txo watches, too: see watch_tx_and_outputs() */
	txw = find_txwatch(channel->peer->ld->topology, &txid, channel);
	if (!txw)
		log_unusual(channel->log, "Can't unwatch txid %s",
			    type_to_string(ltmp, struct bitcoin_txid, &txid));
	tal_free(txw);
}

static void handle_extracted_preimage(struct channel *channel, const u8 *msg)
{
	struct preimage preimage;

	if (!fromwire_onchain_extracted_preimage(msg, &preimage)) {
		channel_internal_error(channel, "Invalid extracted_preimage");
		return;
	}

	onchain_fulfilled_htlc(channel, &preimage);
}

static void handle_missing_htlc_output(struct channel *channel, const u8 *msg)
{
	struct htlc_stub htlc;

	if (!fromwire_onchain_missing_htlc_output(msg, &htlc)) {
		channel_internal_error(channel, "Invalid missing_htlc_output");
		return;
	}

	/* BOLT #5:
	 *
	 * For any committed HTLC which does not have an output in this
	 * commitment transaction, the node MUST fail the corresponding
	 * incoming HTLC (if any) once the commitment transaction has reached
	 * reasonable depth, and MAY fail it sooner if no valid commitment
	 * transaction contains an output corresponding to the HTLC.
	 */
	onchain_failed_our_htlc(channel, &htlc, "missing in commitment tx");
}

static void handle_onchain_htlc_timeout(struct channel *channel, const u8 *msg)
{
	struct htlc_stub htlc;

	if (!fromwire_onchain_htlc_timeout(msg, &htlc)) {
		channel_internal_error(channel, "Invalid onchain_htlc_timeout");
		return;
	}

	/* BOLT #5:
	 *
	 * If the HTLC output has *timed out* and not been *resolved*, the node
	 * MUST *resolve* the output and MUST fail the corresponding incoming
	 * HTLC (if any) once the resolving transaction has reached reasonable
	 * depth.
	 */
	onchain_failed_our_htlc(channel, &htlc, "timed out");
}

static void handle_irrevocably_resolved(struct channel *channel, const u8 *msg)
{
	/* FIXME: Implement check_htlcs to ensure no dangling hout->in ptrs! */
	free_htlcs(channel->peer->ld, channel);

	log_info(channel->log, "onchaind complete, forgetting peer");

	/* This will also free onchaind. */
	delete_channel(channel);
}

/**
 * onchain_add_utxo -- onchaind is telling us about an UTXO we own
 */
static void onchain_add_utxo(struct channel *channel, const u8 *msg)
{
	struct utxo *u = tal(msg, struct utxo);
	u->close_info = tal(u, struct unilateral_close_info);

	u->is_p2sh = true;
	u->keyindex = 0;
	u->status = output_state_available;
	u->close_info->channel_id = channel->dbid;
	u->close_info->peer_id = channel->peer->id;

	if (!fromwire_onchain_add_utxo(msg, &u->txid, &u->outnum,
				       &u->close_info->commitment_point,
				       &u->amount)) {
		fatal("onchaind gave invalid add_utxo message: %s", tal_hex(msg, msg));
	}


	wallet_add_utxo(channel->peer->ld->wallet, u, p2wpkh);
}

static unsigned int onchain_msg(struct subd *sd, const u8 *msg, const int *fds)
{
	enum onchain_wire_type t = fromwire_peektype(msg);

	switch (t) {
	case WIRE_ONCHAIN_INIT_REPLY:
		handle_onchain_init_reply(sd->channel, msg);
		break;

	case WIRE_ONCHAIN_BROADCAST_TX:
		handle_onchain_broadcast_tx(sd->channel, msg);
		break;

	case WIRE_ONCHAIN_UNWATCH_TX:
		handle_onchain_unwatch_tx(sd->channel, msg);
		break;

 	case WIRE_ONCHAIN_EXTRACTED_PREIMAGE:
		handle_extracted_preimage(sd->channel, msg);
		break;

	case WIRE_ONCHAIN_MISSING_HTLC_OUTPUT:
		handle_missing_htlc_output(sd->channel, msg);
		break;

	case WIRE_ONCHAIN_HTLC_TIMEOUT:
		handle_onchain_htlc_timeout(sd->channel, msg);
		break;

	case WIRE_ONCHAIN_ALL_IRREVOCABLY_RESOLVED:
		handle_irrevocably_resolved(sd->channel, msg);
		break;

	case WIRE_ONCHAIN_ADD_UTXO:
		onchain_add_utxo(sd->channel, msg);
		break;

	/* We send these, not receive them */
	case WIRE_ONCHAIN_INIT:
	case WIRE_ONCHAIN_SPENT:
	case WIRE_ONCHAIN_DEPTH:
	case WIRE_ONCHAIN_HTLC:
	case WIRE_ONCHAIN_KNOWN_PREIMAGE:
		break;
	}

	return 0;
}

/* If we want to know if this HTLC is missing, return depth. */
static bool tell_if_missing(const struct channel *channel,
			    struct htlc_stub *stub,
			    bool *tell_immediate)
{
	struct htlc_out *hout;

	/* Keep valgrind happy. */
	*tell_immediate = false;

	/* Is it a current HTLC? */
	hout = find_htlc_out_by_ripemd(channel, &stub->ripemd);
	if (!hout)
		return false;

	/* BOLT #5:
	 *
	 * For any committed HTLC which does not have an output in this
	 * commitment transaction, the node MUST fail the corresponding
	 * incoming HTLC (if any) once the commitment transaction has reached
	 * reasonable depth, and MAY fail it sooner if no valid commitment
	 * transaction contains an output corresponding to the HTLC.
	 */
	if (hout->hstate >= RCVD_ADD_REVOCATION
	    && hout->hstate < SENT_REMOVE_REVOCATION)
		*tell_immediate = true;

	log_debug(channel->log,
		  "We want to know if htlc %"PRIu64" is missing (%s)",
		  hout->key.id, *tell_immediate ? "immediate" : "later");
	return true;
}

/* Only error onchaind can get is if it dies. */
static void onchain_error(struct channel *channel,
			  int peer_fd, int gossip_fd,
			  const struct crypto_state *cs,
			  u64 gossip_index,
			  const struct channel_id *channel_id,
			  const char *desc,
			  const u8 *err_for_them)
{
	/* FIXME: re-launch? */
	log_broken(channel->log, "%s", desc);
}

/* With a reorg, this can get called multiple times; each time we'll kill
 * onchaind (like any other owner), and restart */
enum watch_result funding_spent(struct channel *channel,
				const struct bitcoin_tx *tx,
				size_t input_num,
				const struct block *block)
{
	u8 *msg, *scriptpubkey;
	struct bitcoin_txid our_last_txid;
	s64 keyindex;
	struct pubkey ourkey;
	struct htlc_stub *stubs;
	const tal_t *tmpctx = tal_tmpctx(channel);
	struct lightningd *ld = channel->peer->ld;

	channel_fail_permanent(channel, "Funding transaction spent");

	/* We could come from almost any state. */
	channel_set_state(channel, channel->state, FUNDING_SPEND_SEEN);

	channel_set_owner(channel, new_channel_subd(ld,
						    "lightning_onchaind",
						    channel,
						    channel->log,
						    onchain_wire_type_name,
						    onchain_msg,
						    onchain_error,
						    channel_set_billboard,
						    NULL));

	if (!channel->owner) {
		log_broken(channel->log, "Could not subdaemon onchain: %s",
			   strerror(errno));
		tal_free(tmpctx);
		return KEEP_WATCHING;
	}

	stubs = wallet_htlc_stubs(tmpctx, ld->wallet, channel);
	if (!stubs) {
		log_broken(channel->log, "Could not load htlc_stubs");
		tal_free(tmpctx);
		return KEEP_WATCHING;
	}

	/* We re-use this key to send other outputs to. */
	if (channel->local_shutdown_idx >= 0)
		keyindex = channel->local_shutdown_idx;
	else {
		keyindex = wallet_get_newindex(ld);
		if (keyindex < 0) {
			log_broken(channel->log, "Could not get keyindex");
			tal_free(tmpctx);
			return KEEP_WATCHING;
		}
	}
	scriptpubkey = p2wpkh_for_keyidx(tmpctx, ld, keyindex);
	if (!scriptpubkey) {
		channel_internal_error(channel,
				    "Can't get shutdown script %"PRIu64,
				    keyindex);
		tal_free(tmpctx);
		return DELETE_WATCH;
	}
	txfilter_add_scriptpubkey(ld->owned_txfilter, scriptpubkey);

	if (!bip32_pubkey(ld->wallet->bip32_base, &ourkey, keyindex)) {
		channel_internal_error(channel,
				    "Can't get shutdown key %"PRIu64,
				    keyindex);
		tal_free(tmpctx);
		return DELETE_WATCH;
	}

	/* This could be a mutual close, but it doesn't matter. */
	bitcoin_txid(channel->last_tx, &our_last_txid);

	msg = towire_onchain_init(channel,
				  &channel->seed, &channel->their_shachain.chain,
				  channel->funding_satoshi,
				  &channel->channel_info.old_remote_per_commit,
				  &channel->channel_info.remote_per_commit,
				   /* BOLT #2:
				    * `to_self_delay` is the number of blocks
				    * that the other nodes to-self outputs
				    * must be delayed */
				   /* So, these are reversed: they specify ours,
				    * we specify theirs. */
				  channel->channel_info.their_config.to_self_delay,
				  channel->our_config.to_self_delay,
				  get_feerate(ld->topology, FEERATE_NORMAL),
				  channel->our_config.dust_limit_satoshis,
				  &channel->channel_info.theirbase.revocation,
				  &our_last_txid,
				  scriptpubkey,
				  channel->remote_shutdown_scriptpubkey,
				  &ourkey,
				  channel->funder,
				  &channel->channel_info.theirbase.payment,
				  &channel->channel_info.theirbase.htlc,
				  &channel->channel_info.theirbase.delayed_payment,
				  tx,
				  block->height,
				  /* FIXME: config for 'reasonable depth' */
				  3,
				  channel->last_htlc_sigs,
				  tal_count(stubs));
	subd_send_msg(channel->owner, take(msg));

	/* FIXME: Don't queue all at once, use an empty cb... */
	for (size_t i = 0; i < tal_count(stubs); i++) {
		bool tell_immediate;
		bool tell = tell_if_missing(channel, &stubs[i], &tell_immediate);
		msg = towire_onchain_htlc(channel, &stubs[i],
					  tell, tell_immediate);
		subd_send_msg(channel->owner, take(msg));
	}

	watch_tx_and_outputs(channel, tx);

	tal_free(tmpctx);
	/* We keep watching until peer finally deleted, for reorgs. */
	return KEEP_WATCHING;
}

