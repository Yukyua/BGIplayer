// Original 0x00436E40 returns the forward link at CThread+0x0C.
namespace bgiplayer {
uint32_t VM::next_thread_in_tree(uint32_t id) const {
    if (id == 1) {
        return root_children_.empty() ? 0U : root_children_.front();
    }
    const auto position = std::find(root_children_.begin(), root_children_.end(), id);
    return position != root_children_.end() &&
                   std::next(position) != root_children_.end()
        ? *std::next(position) : 0U;
}
}  // namespace bgiplayer
