/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2019 Intel Corporation
 */

#ifndef __INTEL_HDCP_H__
#define __INTEL_HDCP_H__

#include <linux/types.h>

#define HDCP_ENCRYPT_STATUS_CHANGE_TIMEOUT_MS	50

struct drm_connector;
struct drm_connector_state;
struct intel_atomic_state;
struct intel_connector;
struct intel_crtc_state;
struct intel_digital_port;
struct intel_display;
struct intel_encoder;
struct intel_hdcp_shim;
struct seq_file;
enum port;

void intel_hdcp_atomic_check(struct drm_connector *connector,
			     struct drm_connector_state *old_state,
			     struct drm_connector_state *new_state);
int intel_hdcp_init(struct intel_connector *connector,
		    struct intel_digital_port *dig_port,
		    const struct intel_hdcp_shim *hdcp_shim);
void intel_hdcp_enable(struct intel_atomic_state *state,
		       struct intel_encoder *encoder,
		       const struct intel_crtc_state *pipe_config,
		       const struct drm_connector_state *conn_state);
int intel_hdcp_disable(struct intel_connector *connector);
void intel_hdcp_cancel_works(struct intel_connector *connector);
void intel_hdcp_update_pipe(struct intel_atomic_state *state,
			    struct intel_encoder *encoder,
			    const struct intel_crtc_state *crtc_state,
			    const struct drm_connector_state *conn_state);
bool is_hdcp_supported(struct intel_display *display, enum port port);
void intel_hdcp_component_init(struct intel_display *display);
void intel_hdcp_component_fini(struct intel_display *display);
void intel_hdcp_cleanup(struct intel_connector *connector);
void intel_hdcp_handle_cp_irq(struct intel_connector *connector);

void intel_hdcp_info(struct seq_file *m, struct intel_connector *connector);
void intel_hdcp_connector_debugfs_add(struct intel_connector *connector);

#endif /* __INTEL_HDCP_H__ */
