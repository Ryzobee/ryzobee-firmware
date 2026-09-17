/* Adapter for the existing tool-layer fake RGB worker. This is not a second
 * implementation of the target worker; existing tests drive its completion. */
static uint32_t rgb_fixture_lease, rgb_fixture_next_lease, rgb_fixture_closed_lease;
static uint32_t rgb_fixture_request;
static bool rgb_fixture_closing;

esp_err_t ryz_rgb_lease_open(uint32_t *out)
{
    *out = 0;
    if (rgb_fixture_lease) return ESP_ERR_NOT_FINISHED;
    *out = rgb_fixture_lease = ++rgb_fixture_next_lease;
    rgb_fixture_request = 0;
    rgb_fixture_closing = false;
    return ESP_OK;
}

esp_err_t ryz_rgb_lease_submit(uint32_t lease, ryz_rgb_color_t color, uint32_t *out)
{
    *out = 0;
    if (lease != rgb_fixture_lease || rgb_fixture_closing) return ESP_ERR_NOT_FINISHED;
    esp_err_t error = ryz_rgb_submit(color, out);
    if (error == ESP_OK) rgb_fixture_request = *out;
    return error;
}

esp_err_t ryz_rgb_lease_close(uint32_t lease)
{
    if (lease && lease <= rgb_fixture_closed_lease) return ESP_OK;
    if (!lease || lease != rgb_fixture_lease) return ESP_ERR_INVALID_STATE;
    if (rgb_fixture_request) {
        ryz_rgb_snapshot_t snapshot;
        esp_err_t error = ryz_rgb_get_snapshot(&snapshot);
        if (error != ESP_OK) return error;
        if (snapshot.request_id != rgb_fixture_request) return ESP_ERR_INVALID_STATE;
        if (snapshot.phase != RYZ_RGB_CLEANED || snapshot.resources_held) {
            if (rgb_fixture_closing && snapshot.phase == RYZ_RGB_FAILED)
                return snapshot.cleanup_error != ESP_OK ? snapshot.cleanup_error : ESP_FAIL;
            if (!rgb_fixture_closing) {
                error = ryz_rgb_cleanup(rgb_fixture_request);
                if (error != ESP_OK) return error;
                rgb_fixture_closing = true;
            }
            return ESP_ERR_NOT_FINISHED;
        }
    }
    rgb_fixture_closed_lease = rgb_fixture_lease;
    rgb_fixture_lease = rgb_fixture_request = 0;
    rgb_fixture_closing = false;
    return ESP_OK;
}

esp_err_t ryz_rgb_lease_retry_close(uint32_t lease)
{
    if (!lease || lease != rgb_fixture_lease || !rgb_fixture_closing)
        return ESP_ERR_INVALID_STATE;
    rgb_fixture_closing = false;
    return ESP_OK;
}
