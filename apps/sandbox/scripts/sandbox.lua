return {
    on_spawn = function(actor, level)
        log.info("sandbox script spawned")
    end,

    on_tick = function(actor, level, delta_seconds)
        actor:translate(0.02 * delta_seconds, 0.0, 0.0)
    end,

    on_destroy = function(actor, level)
        log.info("sandbox script destroyed")
    end
}
