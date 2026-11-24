package com.wzmwayne.oledpanel

import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.TextView
import androidx.recyclerview.widget.RecyclerView

/** title 是 .ezzbk 元数据里的书名，空表示容器没带，退回文件名显示 */
data class Book(val name: String, val size: Long, val title: String = "")

class BookAdapter(
    private val onDelete: (Book) -> Unit
) : RecyclerView.Adapter<BookAdapter.VH>() {

    private val items = ArrayList<Book>()

    fun submit(newItems: List<Book>) {
        items.clear()
        items.addAll(newItems)
        notifyDataSetChanged()
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): VH {
        val view = LayoutInflater.from(parent.context)
            .inflate(R.layout.item_book, parent, false)
        return VH(view)
    }

    override fun getItemCount(): Int = items.size

    override fun onBindViewHolder(holder: VH, position: Int) {
        holder.bind(items[position])
    }

    inner class VH(itemView: View) : RecyclerView.ViewHolder(itemView) {
        private val tvName: TextView = itemView.findViewById(R.id.tvBookName)
        private val tvSize: TextView = itemView.findViewById(R.id.tvBookSize)
        private val btnDel: TextView = itemView.findViewById(R.id.btnBookDelete)

        fun bind(book: Book) {
            val label = book.title.ifBlank { book.name }
            tvName.text = label
            /* 显示的是书名时，把真实文件名带上——删除/排查都按文件名走 */
            tvSize.text = if (label == book.name) formatSize(book.size)
                          else book.name + " · " + formatSize(book.size)
            btnDel.setOnClickListener { onDelete(book) }
        }

        private fun formatSize(bytes: Long): String = when {
            bytes >= 1024 * 1024 -> String.format("%.1f MB", bytes / 1024f / 1024f)
            bytes >= 1024 -> String.format("%.1f KB", bytes / 1024f)
            else -> "$bytes B"
        }
    }
}
