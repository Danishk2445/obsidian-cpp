oc_add_library(oc_graph
  SOURCES
    force_simulation.cpp
    graph_data.cpp
    graph_settings.cpp
    parallel.cpp
    quadtree.cpp
    simple_query.cpp
    simulation_runner.cpp
  PUBLIC_DEPS Qt6::Core Qt6::Concurrent
)
